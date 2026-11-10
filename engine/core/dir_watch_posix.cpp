// core/dir_watch.h on Linux and macOS: the tree's files (path, size, time) compared with the last
// look, at most every 300 ms. The watched folders are small (HTML pages), so a scan costs little,
// and it needs no inotify watch per subfolder nor FSEvents' run loop.
#include "core/dir_watch.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <filesystem>
#include <map>

namespace fx {

namespace fs = std::filesystem;

struct DirectoryWatch::Impl {
    struct Stamp {
        uintmax_t size = 0;
        fs::file_time_type time;
        bool operator==(const Stamp& o) const { return size == o.size && time == o.time; }
    };
    fs::path dir;
    bool open = false;
    std::map<std::string, Stamp> files;
    std::vector<std::string> changed;
    uint64_t lastChangeNs = 0, nextScanNs = 0;
    uint64_t retryNs = 0;  // when to look for a missing directory again

    bool scan(std::map<std::string, Stamp>& out) const
    {
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            return false;
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            std::error_code e;
            if (!it->is_regular_file(e))
                continue;
            Stamp stamp;
            stamp.size = it->file_size(e);
            stamp.time = it->last_write_time(e);
            const std::u8string relative = it->path().lexically_relative(dir).generic_u8string();
            out[std::string(relative.begin(), relative.end())] = stamp;
        }
        return true;
    }

    bool start(const std::string& path)
    {
        dir = fs::path(path);
        files.clear();
        open = scan(files);
        return open;
    }

    void add(const std::string& path)
    {
        if (std::find(changed.begin(), changed.end(), path) == changed.end())
            changed.push_back(path);
    }

    void collect(uint64_t now)
    {
        if (now < nextScanNs)
            return;
        nextScanNs = now + 300'000'000ull;
        std::map<std::string, Stamp> current;
        if (!scan(current)) {
            open = false;  // the directory went away: look for it again later
            retryNs = now + 2'000'000'000ull;
            return;
        }
        bool any = false;
        for (const auto& [path, stamp] : current) {
            const auto was = files.find(path);
            if (was == files.end() || !(was->second == stamp)) {
                add(path);
                any = true;
            }
        }
        for (const auto& [path, stamp] : files)
            if (!current.count(path)) {
                add(path);
                any = true;
            }
        files = std::move(current);
        if (any)
            lastChangeNs = now;
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
    if (!impl_->start(directory))
        impl_->retryNs = SDL_GetTicksNS() + 2'000'000'000ull;
}

bool DirectoryWatch::active() const { return impl_ && impl_->open; }

bool DirectoryWatch::poll(std::vector<std::string>& changed, uint32_t settleMs)
{
    if (!impl_)
        return false;
    Impl& w = *impl_;
    const uint64_t now = SDL_GetTicksNS();
    if (!w.open) {
        if (now < w.retryNs)
            return false;
        w.retryNs = now + 2'000'000'000ull;
        if (!w.start(directory_))
            return false;
    }
    w.collect(now);
    if (w.changed.empty() || now - w.lastChangeNs < uint64_t(settleMs) * 1'000'000ull)
        return false;
    changed = std::move(w.changed);
    w.changed.clear();
    return true;
}

}  // namespace fx
