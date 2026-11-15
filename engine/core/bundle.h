#pragma once

// Files carried at the end of an executable: what the engine would otherwise keep in folders beside
// falcon.exe. A release build appends the UI's page bundles (ui/editor/index.html, ...) and the
// engine's content (content/library/..., content/prototype/...) with tools/pack, so falcon.exe is the
// whole editor. The program itself doesn't change: the files sit after it, where the OS's loader
// doesn't look.
//
// The layout: the program's bytes, then each file's bytes, then an index (per file: a u16 path
// length, the path, its u64 offset from the start of the executable, its u64 size, a u64 hash of
// its bytes), then a 48-byte footer: "FXBUNDLE", a u32 version, a u32 file count, the u64 offset
// where the files start (the program's size), the index's u64 offset and u64 size, and the
// bundle's id (a hash of every file's path, size and hash). Little-endian, paths '/'-separated
// UTF-8.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fx::bundle {

struct File {
    std::string path;     // "ui/editor/index.html"
    uint64_t offset = 0;  // from the start of the executable
    uint64_t size = 0;
    uint64_t hash = 0;    // FNV-1a of its bytes
};

class Bundle {
public:
    // Reads `file`'s index; false (and empty) when it ends without one.
    bool open(const std::filesystem::path& file);
    bool empty() const { return files_.empty(); }
    const std::filesystem::path& file() const { return file_; }
    uint64_t id() const { return id_; }
    // Where the files start: the program's size (the file's size without a bundle).
    uint64_t programSize() const { return start_; }
    const std::vector<File>& files() const { return files_; }

    // The file at `path` (no leading '/'), or null.
    const File* find(std::string_view path) const;
    // Whether a file is at or under `folder` ("ui/editor").
    bool has(std::string_view folder) const;
    // A hash of the paths, sizes and bytes of the files under `folder` (unpacked copies are kept by
    // it: they stay valid while those files are the same).
    uint64_t idOf(std::string_view folder) const;
    bool read(const File& f, std::string& out) const;
    bool read(std::string_view path, std::string& out) const;
    // The files under `folder` into the directory `to` (made), as their paths below `folder`.
    bool extract(std::string_view folder, const std::filesystem::path& to, std::string& error) const;

private:
    std::filesystem::path file_;
    std::vector<File> files_;  // sorted by path
    uint64_t start_ = 0, id_ = 0;
};

// What a new bundle holds: a file on disk, or one of another bundle's files.
struct Source {
    std::string path;                // in the bundle
    std::filesystem::path disk;      // or:
    const Bundle* from = nullptr;    // (with `entry`)
    const File* entry = nullptr;
};

// The files of the directory `dir` (recursively) under `prefix` ("ui": dir/editor/index.html is
// ui/editor/index.html).
void addDirectory(std::vector<Source>& out, const std::filesystem::path& dir, const std::string& prefix);

// Writes the program of `exe` (without the bundle it may carry) to `out`, followed by a bundle of
// `files` (none: the program alone). `out` may be `exe`: it is written beside it and moved over it.
bool write(const std::filesystem::path& exe, const std::filesystem::path& out, const std::vector<Source>& files,
           std::string& error);

}  // namespace fx::bundle
