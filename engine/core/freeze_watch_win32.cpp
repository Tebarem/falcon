#include "core/freeze_watch.h"

#include "core/log.h"

#include <SDL3/SDL.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace fx::freezewatch {

namespace {

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
        const uint64_t ms = SDL_GetTicks();
        std::snprintf(time, sizeof(time), "%7.2f", double(ms) * 1e-3);
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_log.push_back(std::string(time) + " " + name + ": " + (message ? message : ""));
        if (g_log.size() > kLogLines)
            g_log.pop_front();
    }
    if (g_previousLog)
        g_previousLog(g_previousLogData, category, priority, message);
}

// ---- Watching --------------------------------------------------------------------------------

std::atomic<uint64_t> g_beatNs{0}, g_beats{0}, g_frame{0};
std::atomic<bool> g_reported{false};
std::atomic<double> g_reportStopped{0.0};
DWORD g_mainThread = 0;
float g_seconds = 8.0f;
std::wstring g_reportDir;  // as given ("" = the default)
std::thread g_watcher;
std::mutex g_waitMutex;
std::condition_variable g_wake;
bool g_stop = false;
std::mutex g_reportMutex;
std::string g_lastReport;

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(std::max(n, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* s, int length = -1)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, length, nullptr, 0, nullptr, nullptr);
    std::string out(size_t(std::max(n, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, length, out.data(), n, nullptr, nullptr);
    if (length < 0 && !out.empty() && out.back() == '\0')
        out.pop_back();
    return out;
}

std::wstring exeDirectory()
{
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
    wchar_t* slash = length ? std::wcsrchr(path, L'\\') : nullptr;
    if (slash)
        slash[1] = 0;
    return path;
}

struct Module {
    DWORD64 base, end;
    std::string name;
};

std::vector<Module> loadedModules()
{
    std::vector<Module> out;
    HMODULE handles[1024];
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &bytes))
        return out;
    const DWORD count = std::min<DWORD>(bytes / sizeof(HMODULE), DWORD(std::size(handles)));
    for (DWORD i = 0; i < count; ++i) {
        MODULEINFO info = {};
        wchar_t name[MAX_PATH] = {};
        if (!GetModuleInformation(GetCurrentProcess(), handles[i], &info, sizeof(info)))
            continue;
        GetModuleBaseNameW(GetCurrentProcess(), handles[i], name, MAX_PATH);
        out.push_back({DWORD64(info.lpBaseOfDll), DWORD64(info.lpBaseOfDll) + info.SizeOfImage, narrow(name)});
    }
    std::sort(out.begin(), out.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
    return out;
}

const Module* moduleOf(DWORD64 pc, const Module* modules, size_t count)
{
    size_t lo = 0, hi = count;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (pc < modules[mid].base)
            hi = mid;
        else if (pc >= modules[mid].end)
            lo = mid + 1;
        else
            return &modules[mid];
    }
    return nullptr;
}

constexpr int kFrames = 64;
struct Stack {
    DWORD thread = 0;
    int count = 0;
    DWORD64 pc[kFrames] = {};
};

// The suspended thread's return addresses, from the images' unwind data. Nothing here may
// allocate or lock (the thread may hold the heap's lock): it stops at code outside the loaded
// images (C#'s JIT code: its unwind data lives in the runtime).
int unwindSuspended(HANDLE thread, DWORD64* pcs, int max, const Module* modules, size_t moduleCount)
{
    alignas(16) CONTEXT context;
    std::memset(&context, 0, sizeof(context));
    context.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &context))
        return 0;
    int n = 0;
    __try {
        while (n < max && context.Rip) {
            pcs[n++] = context.Rip;
            if (!moduleOf(context.Rip, modules, moduleCount))
                break;
            DWORD64 base = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
            if (function) {
                void* handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context, &handlerData, &establisher, nullptr);
            } else {  // a leaf function: the return address is on top of the stack
                context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                context.Rsp += 8;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

// Every thread's stack (the main thread first), or only the main thread's.
std::vector<Stack> captureStacks(bool everyThread, const std::vector<Module>& modules)
{
    std::vector<DWORD> ids;
    ids.push_back(g_mainThread);
    if (everyThread) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry = {};
            entry.dwSize = sizeof(entry);
            const DWORD self = GetCurrentProcessId();
            for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
                if (entry.th32OwnerProcessID == self && entry.th32ThreadID != g_mainThread &&
                    entry.th32ThreadID != GetCurrentThreadId())
                    ids.push_back(entry.th32ThreadID);
            CloseHandle(snapshot);
        }
    }
    std::vector<Stack> stacks(ids.size());  // (allocated before any thread is suspended)
    for (size_t i = 0; i < ids.size(); ++i) {
        stacks[i].thread = ids[i];
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, ids[i]);
        if (!thread)
            continue;
        if (SuspendThread(thread) != DWORD(-1)) {
            stacks[i].count = unwindSuspended(thread, stacks[i].pc, kFrames, modules.data(), modules.size());
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    return stacks;
}

std::string threadName(DWORD id)
{
    std::string name;
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (!thread)
        return name;
    PWSTR description = nullptr;
    if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
        name = narrow(description);
        LocalFree(description);
    }
    CloseHandle(thread);
    return name;
}

// dbghelp, in a session of its own (a duplicated process handle: Tracy may have one too).
struct Symbols {
    HANDLE process = nullptr;
    bool ok = false;
    Symbols()
    {
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &process, 0, FALSE,
                             DUPLICATE_SAME_ACCESS))
            return;
        SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES |
                      SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
        const std::wstring dir = exeDirectory();
        ok = SymInitializeW(process, dir.c_str(), TRUE) != FALSE;
    }
    ~Symbols()
    {
        if (ok)
            SymCleanup(process);
        if (process)
            CloseHandle(process);
    }

    std::string frame(DWORD64 pc, const std::vector<Module>& modules)
    {
        char text[1024];
        const Module* module = moduleOf(pc, modules.data(), modules.size());
        if (!module) {
            std::snprintf(text, sizeof(text), "0x%016llx (C# or other generated code)", static_cast<unsigned long long>(pc));
            return text;
        }
        // (pc - 1: the call, not the instruction after it)
        const DWORD64 at = pc - 1;
        alignas(SYMBOL_INFOW) unsigned char buffer[sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t)] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
        symbol->MaxNameLen = 511;
        DWORD64 offset = 0;
        if (ok && SymFromAddrW(process, at, &offset, symbol)) {
            std::string line;
            IMAGEHLP_LINEW64 source = {};
            source.SizeOfStruct = sizeof(source);
            DWORD column = 0;
            if (SymGetLineFromAddrW64(process, at, &column, &source) && source.FileName)
                line = " [" + narrow(source.FileName) + ":" + std::to_string(source.LineNumber) + "]";
            std::snprintf(text, sizeof(text), "%s!%s+0x%llx%s", module->name.c_str(), narrow(symbol->Name).c_str(),
                          static_cast<unsigned long long>(offset + 1), line.c_str());
        } else {
            std::snprintf(text, sizeof(text), "%s+0x%llx", module->name.c_str(), static_cast<unsigned long long>(pc - module->base));
        }
        return text;
    }
};

void writeStack(std::string& out, const Stack& stack, Symbols& symbols, const std::vector<Module>& modules)
{
    for (int i = 0; i < stack.count; ++i)
        out += "  #" + std::to_string(i) + " " + symbols.frame(stack.pc[i], modules) + "\n";
    if (stack.count == 0)
        out += "  (no stack)\n";
}

bool writeFile(const std::wstring& path, const std::string& text, bool append)
{
    HANDLE file = CreateFileW(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              append ? OPEN_EXISTING : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, text.data(), DWORD(text.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == text.size();
}

// The folder reports go to, made if needed: the one given, else FreezeReports/ beside the
// executable, else under %LOCALAPPDATA%.
std::wstring reportFolder(const std::wstring& stem, std::wstring& path)
{
    std::vector<std::wstring> candidates;
    if (!g_reportDir.empty())
        candidates.push_back(g_reportDir + L"\\");
    else {
        candidates.push_back(exeDirectory() + L"FreezeReports\\");
        wchar_t local[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
            candidates.push_back(std::wstring(local) + L"\\Falcon Engine\\FreezeReports\\");
    }
    for (const std::wstring& dir : candidates) {
        // (each level: CreateDirectoryW makes one)
        for (size_t at = dir.find(L'\\', 3); at != std::wstring::npos; at = dir.find(L'\\', at + 1))
            CreateDirectoryW(dir.substr(0, at).c_str(), nullptr);
        path = dir + stem + L".txt";
        if (writeFile(path, "", false))
            return dir;
    }
    path.clear();
    return {};
}

std::string recentLog()
{
    std::string out;
    std::unique_lock<std::mutex> lock(g_logMutex, std::defer_lock);
    for (int i = 0; i < 100 && !lock.try_lock(); ++i)  // (the main thread can't be holding it long)
        Sleep(10);
    if (!lock.owns_lock())
        return "  (the log was busy)\n";
    for (const std::string& line : g_log)
        out += "  " + line + "\n";
    return out;
}

std::wstring g_reportPath;  // the report of the current freeze (the watcher's own)

void report(double stopped)
{
    const std::vector<Module> modules = loadedModules();
    const std::vector<Stack> stacks = captureStacks(true, modules);  // (first: as close to the freeze as can be)

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    wchar_t stem[64];
    swprintf_s(stem, L"freeze-%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    std::wstring path;
    const std::wstring dir = reportFolder(stem, path);
    if (dir.empty())
        return;

    Symbols symbols;
    char header[1024];
    std::snprintf(header, sizeof(header),
                  "Falcon Engine freeze report\n"
                  "When: %04u-%02u-%02u %02u:%02u:%02u\n"
                  "The main thread had not finished a frame for %.1f s (frame %llu).\n"
                  "Build: %s %s%s\n",
                  now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, stopped,
                  static_cast<unsigned long long>(g_frame.load()), __DATE__, __TIME__,
                  symbols.ok ? "" : " (no symbols: function names need falcon.pdb beside falcon.exe)");
    std::string text = header;
    text += "Executable: " + narrow(exeDirectory().c_str()) + "\n";
    text += "Command line: " + narrow(GetCommandLineW()) + "\n";
    text += "Minidump: " + narrow((std::wstring(stem) + L".dmp").c_str()) + " (beside this file)\n\n";
    for (size_t i = 0; i < stacks.size(); ++i) {
        const std::string name = threadName(stacks[i].thread);
        text += (i == 0 ? "== Main thread " : "== Thread ") + std::to_string(stacks[i].thread) +
                (name.empty() ? "" : " (" + name + ")") + " ==\n";
        writeStack(text, stacks[i], symbols, modules);
        text += "\n";
    }
    text += "== The last log lines ==\n" + recentLog();
    writeFile(path, text, false);
    g_reportPath = path;

    // The minidump: every thread's stack and the memory they point at.
    const std::wstring dumpPath = dir + stem + L".dmp";
    HANDLE dump = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
                          MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                        MiniDumpWithIndirectlyReferencedMemory),
                          nullptr, nullptr, nullptr);
        CloseHandle(dump);
    }
    {
        std::lock_guard<std::mutex> lock(g_reportMutex);
        g_lastReport = narrow(path.c_str());
    }
    g_reportStopped.store(stopped);
    g_reported.store(true);
}

// Still stuck later: where the main thread is now (the same place: a deadlock or a long wait;
// elsewhere: a loop).
void reportAgain(double stopped)
{
    if (g_reportPath.empty())
        return;
    const std::vector<Module> modules = loadedModules();
    const std::vector<Stack> stacks = captureStacks(false, modules);
    Symbols symbols;
    char header[128];
    std::snprintf(header, sizeof(header), "\n== Main thread again, %.1f s into the freeze ==\n", stopped);
    std::string text = header;
    writeStack(text, stacks[0], symbols, modules);
    writeFile(g_reportPath, text, true);
    g_reportStopped.store(stopped);
}

void watch()
{
    SetThreadDescription(GetCurrentThread(), L"Freeze watch");
    uint64_t reportedBeat = ~0ull;
    bool again = false;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(g_waitMutex);
            if (g_wake.wait_for(lock, std::chrono::milliseconds(250), [] { return g_stop; }))
                return;
        }
        const uint64_t at = g_beatNs.load();
        if (at == 0 || IsDebuggerPresent())
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
    g_mainThread = GetCurrentThreadId();
    g_seconds = seconds;
    g_reportDir = widen(reportDir);
    std::replace(g_reportDir.begin(), g_reportDir.end(), L'/', L'\\');
    while (!g_reportDir.empty() && g_reportDir.back() == L'\\')
        g_reportDir.pop_back();
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
        FX_WARN("freeze: the main thread stopped for %.0f s or more; what it was doing is in %s (and a .dmp beside it)",
                g_reportStopped.load(), lastReport().c_str());
}

__declspec(noinline) void testFreeze(uint32_t ms)
{
    FX_INFO("freeze: test: the main thread sleeps %u ms", ms);
    Sleep(ms);
    FX_INFO("freeze: test: awake");  // (also: Sleep isn't a tail call, so this frame shows in the report)
}

std::string lastReport()
{
    std::lock_guard<std::mutex> lock(g_reportMutex);
    return g_lastReport;
}

}  // namespace fx::freezewatch
