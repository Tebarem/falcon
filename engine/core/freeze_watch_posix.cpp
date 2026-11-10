// core/freeze_watch.h on Linux and macOS: the main thread's call stack (a signal makes it record
// its own with backtrace()) and the last log lines. No minidump: a core file is the system's
// business (ulimit -c), and the report says so.
#include "core/freeze_watch.h"

#include "core/log.h"
#include "core/platform.h"

#include <SDL3/SDL.h>

#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace fx::freezewatch {

namespace {

namespace fs = std::filesystem;

// ---- The last log lines --------------------------------------------------------------------

constexpr size_t kLogLines = 200;
std::mutex g_logMutex;
std::deque<std::string> g_log;
SDL_LogOutputFunction g_previousLog = nullptr;
void* g_previousLogData = nullptr;

void SDLCALL logOutput(void*, int category, SDL_LogPriority priority, const char* message)
{
    {
        static const char* const kNames[] = {"", "trace", "verbose", "debug", "info", "warn", "error", "critical"};
        const char* name = priority >= 0 && priority < SDL_LOG_PRIORITY_COUNT ? kNames[priority] : "";
        char time[16];
        std::snprintf(time, sizeof(time), "%7.2f", double(SDL_GetTicks()) * 1e-3);
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_log.push_back(std::string(time) + " " + name + ": " + (message ? message : ""));
        if (g_log.size() > kLogLines)
            g_log.pop_front();
    }
    if (g_previousLog)
        g_previousLog(g_previousLogData, category, priority, message);
}

// ---- The main thread's stack, recorded by itself on a signal ------------------------------

constexpr int kStackSignal = SIGUSR2;
constexpr int kMaxFrames = 64;
void* g_frames[kMaxFrames];
std::atomic<int> g_frameCount{-1};

void onStackSignal(int)
{
    const int saved = errno;
    g_frameCount.store(backtrace(g_frames, kMaxFrames), std::memory_order_release);
    errno = saved;
}

// ---- Watching --------------------------------------------------------------------------------

std::atomic<uint64_t> g_beatNs{0}, g_beats{0}, g_frame{0};
std::atomic<bool> g_reported{false};
std::atomic<double> g_reportStopped{0.0};
pthread_t g_mainThread;
float g_seconds = 8.0f;
fs::path g_reportDir;  // as given ("" = the default)
std::thread g_watcher;
std::mutex g_waitMutex;
std::condition_variable g_wake;
bool g_stop = false;
std::mutex g_reportMutex;
std::string g_lastReport;
fs::path g_reportPath;  // the report of the current freeze (the watcher's own)

std::string mainStack()
{
    g_frameCount.store(-1);
    if (pthread_kill(g_mainThread, kStackSignal) != 0)
        return "  (the main thread could not be asked for its stack)\n";
    for (int i = 0; i < 200 && g_frameCount.load(std::memory_order_acquire) < 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const int count = g_frameCount.load(std::memory_order_acquire);
    if (count < 0)
        return "  (the main thread did not answer)\n";
    std::string out;
    for (int i = 0; i < count; ++i) {
        Dl_info info = {};
        char line[1024];
        if (dladdr(g_frames[i], &info) && info.dli_sname) {
            int status = 0;
            char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
            std::snprintf(line, sizeof(line), "  %2d  %s + 0x%zx  (%s)\n", i, status == 0 && demangled ? demangled : info.dli_sname,
                          size_t(static_cast<char*>(g_frames[i]) - static_cast<char*>(info.dli_saddr)),
                          info.dli_fname ? fs::path(info.dli_fname).filename().c_str() : "?");
            std::free(demangled);
        } else {
            std::snprintf(line, sizeof(line), "  %2d  %p  (%s)\n", i, g_frames[i],
                          info.dli_fname ? fs::path(info.dli_fname).filename().c_str() : "?");
        }
        out += line;
    }
    return out;
}

std::string recentLog()
{
    std::string out;
    std::unique_lock<std::mutex> lock(g_logMutex, std::defer_lock);
    for (int i = 0; i < 100 && !lock.try_lock(); ++i)  // (the main thread can't be holding it long)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!lock.owns_lock())
        return "  (the log was busy)\n";
    for (const std::string& line : g_log)
        out += "  " + line + "\n";
    return out;
}

bool writeFile(const fs::path& path, const std::string& text, bool append)
{
    std::ofstream out(path, append ? std::ios::app : std::ios::trunc);
    out << text;
    return bool(out);
}

// The folder reports go to, made if needed: the one given, else FreezeReports/ beside the
// executable, else under the user's data folder.
fs::path reportFolder(const std::string& stem, fs::path& path)
{
    std::vector<fs::path> candidates;
    if (!g_reportDir.empty())
        candidates.push_back(g_reportDir);
    else {
        candidates.push_back(platform::executableDir() / "FreezeReports");
        if (const char* home = std::getenv("HOME"))
#if defined(__APPLE__)
            candidates.push_back(fs::path(home) / "Library" / "Logs" / "Falcon Engine" / "FreezeReports");
#else
            candidates.push_back(fs::path(home) / ".local" / "share" / "Falcon Engine" / "FreezeReports");
#endif
    }
    for (const fs::path& dir : candidates) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        path = dir / (stem + ".txt");
        if (writeFile(path, "", false))
            return dir;
    }
    path.clear();
    return {};
}

void report(double stopped)
{
    const std::string stack = mainStack();  // (first: as close to the freeze as can be)
    const std::time_t now = std::time(nullptr);
    std::tm local = {};
    localtime_r(&now, &local);
    char stem[64], when[32];
    std::strftime(stem, sizeof(stem), "freeze-%Y%m%d-%H%M%S", &local);
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local);
    fs::path path;
    if (reportFolder(stem, path).empty())
        return;
    char header[512];
    std::snprintf(header, sizeof(header),
                  "Falcon Engine freeze report\n"
                  "When: %s\n"
                  "The main thread had not finished a frame for %.1f s (frame %llu).\n"
                  "Build: %s %s\n",
                  when, stopped, static_cast<unsigned long long>(g_frame.load()), __DATE__, __TIME__);
    std::string text = header;
    text += "Executable: " + platform::utf8(platform::executablePath()) + "\n";
    text += "Process: " + std::to_string(getpid()) + " (no minidump here: attach a debugger, or allow core files)\n\n";
    text += "== Main thread ==\n" + stack + "\n";
    text += "== The last log lines ==\n" + recentLog();
    writeFile(path, text, false);
    g_reportPath = path;
    {
        std::lock_guard<std::mutex> lock(g_reportMutex);
        g_lastReport = platform::utf8(path);
    }
    g_reportStopped.store(stopped);
    g_reported.store(true);
}

// Still stuck later: where the main thread is now.
void reportAgain(double stopped)
{
    if (g_reportPath.empty())
        return;
    char header[128];
    std::snprintf(header, sizeof(header), "\n== Main thread again, %.1f s into the freeze ==\n", stopped);
    writeFile(g_reportPath, header + mainStack(), true);
    g_reportStopped.store(stopped);
}

void watch()
{
    platform::setThreadName("Freeze watch");
    // (the watcher never takes the signal: only the main thread is sent it)
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, kStackSignal);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    uint64_t reportedBeat = ~0ull;
    bool again = false;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(g_waitMutex);
            if (g_wake.wait_for(lock, std::chrono::milliseconds(250), [] { return g_stop; }))
                return;
        }
        const uint64_t at = g_beatNs.load();
        if (at == 0)
            continue;
        const uint64_t beat = g_beats.load();
        const double stopped = double(SDL_GetTicksNS() - at) * 1e-9;
        if (beat != reportedBeat && stopped >= g_seconds) {
            reportedBeat = beat;
            again = false;
            g_reportPath.clear();
            report(stopped);
        } else if (beat == reportedBeat && !again && stopped >= g_seconds + 12.0) {
            again = true;
            reportAgain(stopped);
        }
    }
}

}  // namespace

void keepLog()
{
    static bool kept = false;
    if (kept)
        return;
    kept = true;
    SDL_GetLogOutputFunction(&g_previousLog, &g_previousLogData);
    SDL_SetLogOutputFunction(&logOutput, nullptr);
}

void start(float seconds, const std::string& reportDir)
{
    if (g_watcher.joinable() || seconds <= 0.0f)
        return;
    g_mainThread = pthread_self();
    g_seconds = seconds;
    g_reportDir = fs::path(reportDir);
    struct sigaction action = {};
    action.sa_handler = &onStackSignal;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(kStackSignal, &action, nullptr);
    void* warm[1];
    backtrace(warm, 1);  // (loads the unwinder now, not inside the signal handler)
    g_stop = false;
    g_watcher = std::thread(watch);
}

void stop()
{
    if (!g_watcher.joinable())
        return;
    {
        std::lock_guard<std::mutex> lock(g_waitMutex);
        g_stop = true;
    }
    g_wake.notify_all();
    g_watcher.join();
    g_beatNs.store(0);
}

void heartbeat(uint64_t frame)
{
    g_frame.store(frame, std::memory_order_relaxed);
    g_beatNs.store(SDL_GetTicksNS(), std::memory_order_relaxed);
    g_beats.fetch_add(1, std::memory_order_relaxed);
    if (g_reported.exchange(false))
        FX_WARN("freeze: the main thread stopped for %.0f s or more; what it was doing is in %s", g_reportStopped.load(),
                lastReport().c_str());
}

__attribute__((noinline)) void testFreeze(uint32_t ms)
{
    FX_INFO("freeze: test: the main thread sleeps %u ms", ms);
    // (in steps: the stack signal interrupts a sleep)
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    FX_INFO("freeze: test: awake");
}

std::string lastReport()
{
    std::lock_guard<std::mutex> lock(g_reportMutex);
    return g_lastReport;
}

}  // namespace fx::freezewatch
