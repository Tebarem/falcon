#include "app/process.h"

#include "core/log.h"

#include <SDL3/SDL.h>

#include <windows.h>

namespace fx {

namespace {

std::wstring wide(const std::string& utf8)
{
    if (utf8.empty())
        return {};
    std::wstring out(size_t(MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), int(out.size()));
    return out;
}

// One argument as CommandLineToArgvW reads it back (quotes, and the backslashes before them).
void appendArgument(std::wstring& line, const std::wstring& arg)
{
    if (!line.empty())
        line += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += arg;
        return;
    }
    line += L'"';
    for (size_t i = 0;; ++i) {
        size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++slashes;
        }
        if (i == arg.size()) {
            line.append(slashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"')
            line.append(slashes * 2 + 1, L'\\');
        else
            line.append(slashes, L'\\');
        line += arg[i];
    }
    line += L'"';
}

// `program` with `args`, its output going where ours does. The child inherits our stdout and
// stderr and nothing else (SDL_CreateProcess would hand it every inheritable handle, Chromium's
// IPC with its helpers included). `errorFile`: its error output goes there instead; with
// `allToFile`, its standard output too. A program without a path is searched for (PATH).
HANDLE spawnProgram(const std::string& program, const std::vector<std::string>& args, const std::string& errorFile = {},
                    bool allToFile = false)
{
    std::wstring line;
    appendArgument(line, wide(program));
    for (const std::string& a : args)
        appendArgument(line, wide(a));

    std::vector<HANDLE> inherit;
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    const auto dup = [&](DWORD which) {
        HANDLE h = GetStdHandle(which), copy = nullptr;
        if (h && h != INVALID_HANDLE_VALUE && DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            inherit.push_back(copy);
            return copy;
        }
        return HANDLE(nullptr);
    };
    si.StartupInfo.hStdOutput = allToFile ? nullptr : dup(STD_OUTPUT_HANDLE);
    if (errorFile.empty()) {
        si.StartupInfo.hStdError = dup(STD_ERROR_HANDLE);
    } else {
        SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
        HANDLE f = CreateFileW(wide(errorFile).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, &sa, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            inherit.push_back(f);
            si.StartupInfo.hStdError = f;
            if (allToFile)
                si.StartupInfo.hStdOutput = f;
        }
    }
    if (!inherit.empty())
        si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<uint8_t> attributes(size);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    bool listed = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size) &&
                  (inherit.empty() || UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                                                                inherit.size() * sizeof(HANDLE), nullptr, nullptr));
    PROCESS_INFORMATION pi = {};
    const bool ok = listed && CreateProcessW(nullptr, line.data(), nullptr, nullptr, !inherit.empty(),
                                             EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
                                             &si.StartupInfo, &pi);
    const DWORD error = GetLastError();
    if (listed)
        DeleteProcThreadAttributeList(si.lpAttributeList);
    for (HANDLE h : inherit)
        CloseHandle(h);
    if (!ok) {
        FX_ERROR("process: cannot start %s (error %lu)", program.c_str(), error);
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

HANDLE spawn(const std::vector<std::string>& args, const std::string& errorFile = {})
{
    return spawnProgram(falconExe(), args, errorFile);
}

}  // namespace

std::string falconExe()
{
    const char* base = SDL_GetBasePath();
    return std::string(base ? base : "") + "falcon.exe";
}

int runFalcon(const std::vector<std::string>& args)
{
    HANDLE p = spawn(args);
    if (!p)
        return -1;
    DWORD code = DWORD(-1);
    if (WaitForSingleObject(p, INFINITE) != WAIT_OBJECT_0 || !GetExitCodeProcess(p, &code))
        code = DWORD(-1);
    CloseHandle(p);
    return int(code);
}

void* startFalconTracked(const std::vector<std::string>& args, const std::string& errorFile) { return spawn(args, errorFile); }

bool openConsole(const std::vector<std::string>& args)
{
    if (args.empty())
        return false;
    std::wstring line;
    for (const std::string& a : args)
        appendArgument(line, wide(a));
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    // (a window of its own, nothing inherited: the player types into it)
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
        FX_ERROR("process: cannot start %s (error %lu)", args[0].c_str(), GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void* startProgramTracked(const std::string& program, const std::vector<std::string>& args, const std::string& outputFile)
{
    return spawnProgram(program, args, outputFile, true);
}

uint32_t currentProcessId() { return uint32_t(GetCurrentProcessId()); }

bool processRunning(uint32_t pid)
{
    if (!pid)
        return false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, DWORD(pid));
    if (!p)
        return GetLastError() == ERROR_ACCESS_DENIED;  // it exists, someone else's
    const bool running = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
    CloseHandle(p);
    return running;
}

bool falconFinished(void* process, int& code)
{
    if (!process || WaitForSingleObject(process, 0) != WAIT_OBJECT_0)
        return false;
    DWORD exit = DWORD(-1);
    GetExitCodeProcess(process, &exit);
    code = int(exit);
    CloseHandle(process);
    return true;
}

void stopFalcon(void* process)
{
    if (!process)
        return;
    TerminateProcess(process, 1);
    CloseHandle(process);
}

bool processShowsWindow(void* process)
{
    if (!process)
        return false;
    struct Search {
        DWORD pid;
        bool shown;
    } search = {GetProcessId(process), false};
    EnumWindows(
        [](HWND window, LPARAM data) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(data);
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            s.shown = pid == s.pid && IsWindowVisible(window);
            return !s.shown;
        },
        LPARAM(&search));
    return search.shown;
}

void releaseFalcon(void* process)
{
    if (process)
        CloseHandle(process);
}

bool startFalcon(const std::vector<std::string>& args)
{
    HANDLE p = spawn(args);
    if (!p)
        return false;
    CloseHandle(p);  // (the process runs on)
    return true;
}

}  // namespace fx
