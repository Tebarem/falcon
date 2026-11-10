#include "core/platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <share.h>
namespace fx::platform {

namespace {

thread_local DWORD t_lastError = 0;

std::wstring wide(std::string_view utf8)
{
    if (utf8.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), n);
    return out;
}

ULONGLONG fileTimeTicks(const FILETIME& ft) { return (ULONGLONG(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

}  // namespace

std::filesystem::path executablePath()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        if (n == 0)
            return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::filesystem::path executableDir() { return executablePath().parent_path(); }

void* loadLibrary(const std::filesystem::path& path)
{
    // LOAD_WITH_ALTERED_SEARCH_PATH: the library's own imports resolve in its folder first.
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module)
        t_lastError = GetLastError();
    return module;
}

void* librarySymbol(void* library, const char* name)
{
    return library ? reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name)) : nullptr;
}

void freeLibrary(void* library)
{
    if (library)
        FreeLibrary(static_cast<HMODULE>(library));
}

std::string lastError() { return "error " + std::to_string(t_lastError); }

std::string libraryFileName(std::string_view stem) { return std::string(stem) + ".dll"; }

void setThreadName(std::string_view name) { SetThreadDescription(GetCurrentThread(), wide(name).c_str()); }

uint32_t processId() { return uint32_t(GetCurrentProcessId()); }

ProcessMemory processMemory()
{
    ProcessMemory m;
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        m.residentBytes = pmc.WorkingSetSize;
        m.privateBytes = pmc.PrivateUsage;
    }
    return m;
}

double msSinceProcessStart()
{
    FILETIME creation, exit, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        return 0.0;
    GetSystemTimePreciseAsFileTime(&now);
    return double(fileTimeTicks(now) - fileTimeTicks(creation)) / 10000.0;  // 100ns ticks
}

std::filesystem::path documentsDir()
{
    PWSTR documents = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) && documents)
        out = documents;
    CoTaskMemFree(documents);
    return out;
}

bool moveToTrash(const std::vector<std::filesystem::path>& paths)
{
    if (paths.empty())
        return false;
    // SHFileOperation takes full Windows paths, double-NUL terminated.
    std::wstring list;
    for (const std::filesystem::path& p : paths)
        list += p.lexically_normal().make_preferred().wstring() + L'\0';
    list += L'\0';
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = list.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

void revealInFileBrowser(const std::filesystem::path& path)
{
    const std::wstring args = L"/select,\"" + path.lexically_normal().make_preferred().wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

FILE* openFile(const std::filesystem::path& path, const char* mode)
{
    return _wfsopen(path.c_str(), wide(mode).c_str(), _SH_DENYNO);  // shared, like fopen
}

void setEnv(const char* name, const std::string& value) { SetEnvironmentVariableW(wide(name).c_str(), wide(value).c_str()); }

bool hasEnv(const char* name) { return GetEnvironmentVariableW(wide(name).c_str(), nullptr, 0) != 0; }

}  // namespace fx::platform
