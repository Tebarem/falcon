// libcef.dll (delay-loaded) lives in cef/ next to falcon.exe, with Chromium's data: load it from
// there by full path, so its own imports (chrome_elf.dll) resolve in that folder as well. The hook
// is in the executable (not the engine library) so the linker always keeps it; the default DLL
// search order is left alone (hostfxr, script DLLs, GPU drivers).

#include <windows.h>

#include <delayimp.h>

#include <cwchar>

namespace {

FARPROC WINAPI loadCef(unsigned notify, PDelayLoadInfo info)
{
    if (notify != dliNotePreLoadLibrary || _stricmp(info->szDll, "libcef.dll") != 0)
        return nullptr;
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, DWORD(MAX_PATH * 4));
    if (length == 0 || length >= MAX_PATH * 4)
        return nullptr;
    wchar_t* slash = std::wcsrchr(path, L'\\');
    if (!slash || wcscpy_s(slash + 1, size_t(MAX_PATH * 4) - size_t(slash + 1 - path), L"cef\\libcef.dll") != 0)
        return nullptr;
    return reinterpret_cast<FARPROC>(LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
}

}  // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = loadCef;
