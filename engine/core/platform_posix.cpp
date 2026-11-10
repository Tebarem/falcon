// core/platform.h on Linux and macOS.
#include "core/platform.h"

#include <dlfcn.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

extern char** environ;

namespace fx::platform {

namespace fs = std::filesystem;

namespace {

thread_local std::string t_lastError;

fs::path home()
{
    const char* h = std::getenv("HOME");
    return h && *h ? fs::path(h) : fs::path();
}

// `program` with `args`, not waited for (its exit is reaped by nobody: it outlives this process
// or ends with it). False: it didn't start.
bool spawnDetached(const char* program, const std::vector<std::string>& args)
{
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program));
    for (const std::string& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, program, nullptr, nullptr, argv.data(), environ) != 0)
        return false;
    return true;
}

}  // namespace

fs::path executablePath()
{
    static const fs::path path = [] {
#if defined(__APPLE__)
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string buffer(size, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0)
            return fs::path();
        buffer.resize(std::strlen(buffer.c_str()));
        std::error_code ec;
        const fs::path real = fs::canonical(buffer, ec);
        return ec ? fs::path(buffer) : real;
#else
        std::error_code ec;
        const fs::path real = fs::read_symlink("/proc/self/exe", ec);
        return ec ? fs::path() : real;
#endif
    }();
    return path;
}

fs::path executableDir() { return executablePath().parent_path(); }

void* loadLibrary(const fs::path& path)
{
    void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        const char* error = dlerror();
        t_lastError = error ? error : "dlopen failed";
    }
    return library;
}

void* librarySymbol(void* library, const char* name) { return library ? dlsym(library, name) : nullptr; }

void freeLibrary(void* library)
{
    if (library)
        dlclose(library);
}

std::string lastError() { return t_lastError; }

std::string libraryFileName(std::string_view stem)
{
#if defined(__APPLE__)
    return "lib" + std::string(stem) + ".dylib";
#else
    return "lib" + std::string(stem) + ".so";
#endif
}

void setThreadName(std::string_view name)
{
#if defined(__APPLE__)
    pthread_setname_np(std::string(name).c_str());
#else
    // (Linux: at most 15 characters)
    pthread_setname_np(pthread_self(), std::string(name.substr(0, 15)).c_str());
#endif
}

uint32_t processId() { return uint32_t(getpid()); }

ProcessMemory processMemory()
{
    ProcessMemory m;
#if defined(__APPLE__)
    mach_task_basic_info_data_t basic = {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, task_info_t(&basic), &count) == KERN_SUCCESS)
        m.residentBytes = basic.resident_size;
    task_vm_info_data_t vm = {};
    count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, task_info_t(&vm), &count) == KERN_SUCCESS)
        m.privateBytes = vm.phys_footprint;  // (what Activity Monitor shows as Memory)
#else
    // /proc/self/statm: size resident shared text lib data dt (pages)
    std::ifstream statm("/proc/self/statm");
    uint64_t size = 0, resident = 0, shared = 0;
    if (statm >> size >> resident >> shared) {
        const uint64_t page = uint64_t(sysconf(_SC_PAGESIZE));
        m.residentBytes = resident * page;
        m.privateBytes = (resident > shared ? resident - shared : 0) * page;
    }
#endif
    return m;
}

double msSinceProcessStart()
{
#if defined(__APPLE__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, int(getpid())};
    kinfo_proc info = {};
    size_t size = sizeof(info);
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0)
        return 0.0;
    timeval now = {};
    gettimeofday(&now, nullptr);
    const timeval start = info.kp_proc.p_starttime;
    return double(now.tv_sec - start.tv_sec) * 1000.0 + double(now.tv_usec - start.tv_usec) / 1000.0;
#else
    // /proc/self/stat field 22: the start time in clock ticks after boot; /proc/uptime: now.
    std::ifstream statFile("/proc/self/stat");
    std::string stat((std::istreambuf_iterator<char>(statFile)), std::istreambuf_iterator<char>());
    const size_t close = stat.rfind(')');  // (the name may hold spaces)
    if (close == std::string::npos)
        return 0.0;
    std::istringstream fields(stat.substr(close + 2));
    std::string field;
    for (int i = 3; i < 22 && fields >> field; ++i) {
    }
    unsigned long long startTicks = 0;
    if (!(fields >> startTicks))
        return 0.0;
    std::ifstream uptimeFile("/proc/uptime");
    double uptime = 0.0;
    if (!(uptimeFile >> uptime))
        return 0.0;
    return (uptime - double(startTicks) / double(sysconf(_SC_CLK_TCK))) * 1000.0;
#endif
}

fs::path documentsDir()
{
    const fs::path h = home();
    if (h.empty())
        return {};
#if !defined(__APPLE__)
    // The desktop's own name for it (~/.config/user-dirs.dirs: XDG_DOCUMENTS_DIR="$HOME/Dokumente").
    const char* config = std::getenv("XDG_CONFIG_HOME");
    std::ifstream dirs((config && *config ? fs::path(config) : h / ".config") / "user-dirs.dirs");
    for (std::string line; std::getline(dirs, line);) {
        const std::string key = "XDG_DOCUMENTS_DIR=\"";
        if (line.rfind(key, 0) != 0 || line.size() < key.size() + 1)
            continue;
        std::string value = line.substr(key.size(), line.size() - key.size() - 1);
        if (value.rfind("$HOME", 0) == 0)
            value = utf8(h) + value.substr(5);
        if (!value.empty())
            return fs::path(value);
    }
#endif
    return h / "Documents";
}

bool moveToTrash(const std::vector<fs::path>& paths)
{
    const fs::path h = home();
    if (h.empty())
        return false;
#if defined(__APPLE__)
    // (~/.Trash: Finder empties it; "Put Back" needs NSFileManager, which the editor doesn't need)
    const fs::path files = h / ".Trash";
    const fs::path info;
#else
    // The freedesktop.org trash: the file in files/, where it came from in info/<name>.trashinfo.
    const char* data = std::getenv("XDG_DATA_HOME");
    const fs::path trash = (data && *data ? fs::path(data) : h / ".local" / "share") / "Trash";
    const fs::path files = trash / "files";
    const fs::path info = trash / "info";
#endif
    std::error_code ec;
    fs::create_directories(files, ec);
    if (!info.empty())
        fs::create_directories(info, ec);
    bool all = !paths.empty();
    for (const fs::path& path : paths) {
        const fs::path absolute = fs::absolute(path, ec);
        std::string name = utf8(absolute.filename());
        for (int n = 2; fs::exists(files / name, ec) || (!info.empty() && fs::exists(info / (name + ".trashinfo"), ec)); ++n)
            name = utf8(absolute.stem()) + " " + std::to_string(n) + utf8(absolute.extension());
        if (!info.empty()) {
            char when[32] = {};
            const std::time_t now = std::time(nullptr);
            std::tm local = {};
            localtime_r(&now, &local);
            std::strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%S", &local);
            std::ofstream out(info / (name + ".trashinfo"));
            out << "[Trash Info]\nPath=" << utf8(absolute) << "\nDeletionDate=" << when << "\n";
        }
        fs::rename(absolute, files / name, ec);  // (across file systems this fails: nothing moves)
        if (ec) {
            all = false;
            if (!info.empty())
                fs::remove(info / (name + ".trashinfo"), ec);
        }
    }
    return all;
}

void revealInFileBrowser(const fs::path& path)
{
#if defined(__APPLE__)
    spawnDetached("open", {"-R", utf8(path)});
#else
    // (xdg-open opens the folder; selecting the file would need each file manager's own way)
    spawnDetached("xdg-open", {utf8(fs::is_directory(path) ? path : path.parent_path())});
#endif
}

FILE* openFile(const fs::path& path, const char* mode) { return std::fopen(path.c_str(), mode); }

void setEnv(const char* name, const std::string& value) { ::setenv(name, value.c_str(), 1); }

bool hasEnv(const char* name)
{
    const char* value = std::getenv(name);
    return value && *value;
}

}  // namespace fx::platform
