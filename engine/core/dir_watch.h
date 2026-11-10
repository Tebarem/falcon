#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fx {

// Files changing under a directory (and its subdirectories), from Windows' change notifications
// (ReadDirectoryChangesW, overlapped): polled once a frame, it reports what changed once nothing
// more has changed for a moment, so an editor's save (often a temporary file and a rename) or a
// copy of many files comes as one change. A directory that isn't there yet is looked for again
// every two seconds. Used to reload HTML pages when their files change.
class DirectoryWatch {
public:
    DirectoryWatch();
    ~DirectoryWatch();
    DirectoryWatch(const DirectoryWatch&) = delete;
    DirectoryWatch& operator=(const DirectoryWatch&) = delete;

    void watch(const std::string& directory);  // "" stops
    const std::string& directory() const { return directory_; }
    bool active() const;                        // the directory is there and watched

    // true once files changed and then nothing for `settleMs`: `changed` gets their paths relative
    // to the directory ('/' separators; "" when Windows lost track of which). Repeats are left out.
    bool poll(std::vector<std::string>& changed, uint32_t settleMs = 150);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string directory_;
};

}  // namespace fx
