#include "core/dir_watch.h"

#include <SDL3/SDL_timer.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>

namespace fx {

struct DirectoryWatch::Impl {
    HANDLE dir = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED overlapped = {};
    bool reading = false;
    alignas(DWORD) unsigned char buffer[32 * 1024];
    std::vector<std::string> changed;
    uint64_t lastChangeNs = 0;
    uint64_t retryNs = 0;  // when to look for a missing directory again

    ~Impl() { close(); }

    void close()
    {
        if (dir != INVALID_HANDLE_VALUE) {
            CancelIoEx(dir, &overlapped);
            if (reading) {
                DWORD bytes = 0;
                GetOverlappedResult(dir, &overlapped, &bytes, TRUE);  // (the buffer is ours until then)
            }
            CloseHandle(dir);
        }
        dir = INVALID_HANDLE_VALUE;
        reading = false;
        if (event)
            CloseHandle(event);
        event = nullptr;
    }

    bool open(const std::string& path)
    {
        const int length = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (length <= 0)
            return false;
        std::wstring wide(size_t(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), length);
        dir = CreateFileW(wide.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (dir == INVALID_HANDLE_VALUE)
            return false;
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event || !read()) {
            close();
            return false;
        }
        return true;
    }

    bool read()
    {
        overlapped = {};
        overlapped.hEvent = event;
        reading = ReadDirectoryChangesW(dir, buffer, DWORD(sizeof(buffer)), TRUE,
                                        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                                        nullptr, &overlapped, nullptr) != 0;
        return reading;
    }

    void add(std::string path)
    {
        if (std::find(changed.begin(), changed.end(), path) == changed.end())
            changed.push_back(std::move(path));
    }

    // What arrived since the last poll (several reads' worth if they did).
    void collect()
    {
        while (reading) {
            DWORD bytes = 0;
            if (!GetOverlappedResult(dir, &overlapped, &bytes, FALSE)) {
                if (GetLastError() == ERROR_IO_INCOMPLETE)
                    return;  // nothing yet
                reading = false;  // the directory went away: look for it again later
                break;
            }
            reading = false;
            lastChangeNs = SDL_GetTicksNS();
            if (bytes == 0) {
                add("");  // too many changes for the buffer: which ones is lost
            } else {
                for (size_t at = 0;;) {
                    const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + at);
                    const int wideLength = int(info->FileNameLength / sizeof(WCHAR));
                    const int length = WideCharToMultiByte(CP_UTF8, 0, info->FileName, wideLength, nullptr, 0, nullptr, nullptr);
                    std::string path(size_t(std::max(length, 0)), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, info->FileName, wideLength, path.data(), length, nullptr, nullptr);
                    std::replace(path.begin(), path.end(), '\\', '/');
                    add(std::move(path));
                    if (!info->NextEntryOffset)
                        break;
                    at += info->NextEntryOffset;
                }
            }
            if (!read())
                break;
        }
        if (!reading)
            close();
    }
};

DirectoryWatch::DirectoryWatch() = default;
DirectoryWatch::~DirectoryWatch() = default;

void DirectoryWatch::watch(const std::string& directory)
{
    if (directory == directory_ && impl_)
        return;
    impl_.reset();
    directory_ = directory;
    if (directory.empty())
        return;
    impl_ = std::make_unique<Impl>();
    if (!impl_->open(directory))
        impl_->retryNs = SDL_GetTicksNS() + 2'000'000'000ull;
}

bool DirectoryWatch::active() const
{
    return impl_ && impl_->dir != INVALID_HANDLE_VALUE;
}

bool DirectoryWatch::poll(std::vector<std::string>& changed, uint32_t settleMs)
{
    if (!impl_)
        return false;
    Impl& w = *impl_;
    const uint64_t now = SDL_GetTicksNS();
    if (w.dir == INVALID_HANDLE_VALUE) {
        if (now < w.retryNs)
            return false;
        w.retryNs = now + 2'000'000'000ull;
        if (!w.open(directory_))
            return false;
    }
    w.collect();
    if (w.changed.empty() || now - w.lastChangeNs < uint64_t(settleMs) * 1'000'000ull)
        return false;
    changed = std::move(w.changed);
    w.changed.clear();
    return true;
}

}  // namespace fx
