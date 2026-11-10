#pragma once

// What the engine asks of the operating system outside the files that belong to one OS
// (process_*.cpp, dir_watch_*.cpp, system_stats_*.cpp, freeze_watch_*.cpp):
// platform_win32.cpp on Windows, platform_posix.cpp on Linux and macOS.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fx::platform {

// This executable, and the folder it is in.
std::filesystem::path executablePath();
std::filesystem::path executableDir();

// A shared library by full path, its own dependencies found beside it; null when it didn't load
// (lastError() says why).
void* loadLibrary(const std::filesystem::path& path);
void* librarySymbol(void* library, const char* name);
void freeLibrary(void* library);
// The last loadLibrary failure, as text.
std::string lastError();

// A shared library's file name here: "phonon" -> phonon.dll, libphonon.so, libphonon.dylib.
std::string libraryFileName(std::string_view stem);

// Names the calling thread for debuggers and profilers.
void setThreadName(std::string_view name);

uint32_t processId();

struct ProcessMemory {
    uint64_t residentBytes = 0;  // in RAM now (working set)
    uint64_t privateBytes = 0;   // committed and not shared
};
ProcessMemory processMemory();

// Milliseconds since this process started.
double msSinceProcessStart();

// The user's Documents folder (empty if unknown).
std::filesystem::path documentsDir();

// Into the Recycle Bin / Trash, undoable from there. False: nothing or not everything moved.
bool moveToTrash(const std::vector<std::filesystem::path>& paths);
// Opens the file browser at `path`'s folder with it selected.
void revealInFileBrowser(const std::filesystem::path& path);

// A path as UTF-8 with this OS's separators (path::string() is the ANSI code page on Windows).
inline std::string utf8(const std::filesystem::path& path)
{
    const std::u8string s = path.u8string();
    return std::string(s.begin(), s.end());
}

// fopen with a path that may hold any characters ("wx": create only, fail if it exists).
FILE* openFile(const std::filesystem::path& path, const char* mode);

// What executables and shared libraries end in here: ".exe" / "" and ".dll" / ".so" / ".dylib"
// (NativeAOT names a library Game.so, without libraryFileName's "lib").
constexpr const char* executableSuffix()
{
#if defined(_WIN32)
    return ".exe";
#else
    return "";
#endif
}
constexpr const char* sharedLibrarySuffix()
{
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

// .NET's runtime identifier for this OS and CPU (dotnet publish -r).
constexpr const char* dotnetRid()
{
#if defined(_WIN32)
    return "win-x64";
#elif defined(__APPLE__)
    return "osx-arm64";
#elif defined(__aarch64__)
    return "linux-arm64";
#else
    return "linux-x64";
#endif
}

// Sets an environment variable for this process and the ones it starts.
void setEnv(const char* name, const std::string& value);
// Whether it is set to something.
bool hasEnv(const char* name);

}  // namespace fx::platform
