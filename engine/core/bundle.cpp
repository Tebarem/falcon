#include "core/bundle.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <system_error>

namespace fx::bundle {

namespace fs = std::filesystem;

namespace {

constexpr char kMagic[8] = {'F', 'X', 'B', 'U', 'N', 'D', 'L', 'E'};
constexpr uint32_t kVersion = 2;
constexpr size_t kFooterSize = 48;
constexpr size_t kChunk = size_t(1) << 20;

fs::path toPath(std::string_view utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

std::string fromPath(const fs::path& p)
{
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

uint64_t getU64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | p[i];
    return v;
}

uint32_t getU32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

void putU64(std::string& s, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        s += char(uint8_t(v >> (8 * i)));
}

void putU32(std::string& s, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        s += char(uint8_t(v >> (8 * i)));
}

// FNV-1a, 64 bits.
struct Hash {
    uint64_t h = 0xcbf29ce484222325ull;
    void add(const void* data, size_t n)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i)
            h = (h ^ p[i]) * 0x100000001b3ull;
    }
};

// Copies `size` bytes at `offset` of `in` to `out` (and the hash).
bool copyRange(std::ifstream& in, uint64_t offset, uint64_t size, std::ofstream& out, Hash* hash, std::vector<char>& buf)
{
    in.clear();
    in.seekg(std::streamoff(offset));
    while (size > 0 && in) {
        const size_t n = size_t(std::min<uint64_t>(size, buf.size()));
        in.read(buf.data(), std::streamsize(n));
        if (size_t(in.gcount()) != n)
            return false;
        if (hash)
            hash->add(buf.data(), n);
        out.write(buf.data(), std::streamsize(n));
        size -= n;
    }
    return size == 0 && bool(out);
}

// "a/b" is `folder` or below it.
bool under(std::string_view path, std::string_view folder)
{
    if (folder.empty())
        return true;
    return path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 && path[folder.size()] == '/';
}

}  // namespace

bool Bundle::open(const fs::path& file)
{
    file_ = file;
    files_.clear();
    start_ = id_ = 0;
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in)
        return false;
    const uint64_t size = uint64_t(in.tellg());
    if (size < kFooterSize)
        return false;
    uint8_t footer[kFooterSize];
    in.seekg(std::streamoff(size - kFooterSize));
    if (!in.read(reinterpret_cast<char*>(footer), kFooterSize) || std::memcmp(footer, kMagic, 8) != 0 || getU32(footer + 8) != kVersion)
        return false;
    const uint32_t count = getU32(footer + 12);
    const uint64_t start = getU64(footer + 16), indexOffset = getU64(footer + 24), indexSize = getU64(footer + 32);
    if (start > indexOffset || indexOffset + indexSize + kFooterSize != size || indexSize > (uint64_t(64) << 20))
        return false;
    std::string index(size_t(indexSize), '\0');
    in.seekg(std::streamoff(indexOffset));
    if (!in.read(index.data(), std::streamsize(indexSize)))
        return false;
    const auto* p = reinterpret_cast<const uint8_t*>(index.data());
    const uint8_t* end = p + index.size();
    std::vector<File> files;
    files.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (end - p < 2)
            return false;
        const size_t len = size_t(p[0]) | size_t(p[1]) << 8;
        p += 2;
        if (size_t(end - p) < len + 24)
            return false;
        File f;
        f.path.assign(reinterpret_cast<const char*>(p), len);
        p += len;
        f.offset = getU64(p);
        f.size = getU64(p + 8);
        f.hash = getU64(p + 16);
        p += 24;
        if (f.offset < start || f.offset + f.size > indexOffset)
            return false;
        files.push_back(std::move(f));
    }
    std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.path < b.path; });
    files_ = std::move(files);
    start_ = start;
    id_ = getU64(footer + 40);
    return true;
}

const File* Bundle::find(std::string_view path) const
{
    const auto it = std::lower_bound(files_.begin(), files_.end(), path, [](const File& f, std::string_view p) { return f.path < p; });
    return it != files_.end() && it->path == path ? &*it : nullptr;
}

bool Bundle::has(std::string_view folder) const
{
    const auto it = std::lower_bound(files_.begin(), files_.end(), folder, [](const File& f, std::string_view p) { return f.path < p; });
    return it != files_.end() && (it->path == folder || under(it->path, folder));
}

uint64_t Bundle::idOf(std::string_view folder) const
{
    Hash h;
    for (const File& f : files_)
        if (under(f.path, folder)) {
            h.add(f.path.data(), f.path.size());
            h.add(&f.size, sizeof f.size);
            h.add(&f.hash, sizeof f.hash);
        }
    return h.h;
}

bool Bundle::read(const File& f, std::string& out) const
{
    std::ifstream in(file_, std::ios::binary);
    if (!in)
        return false;
    out.resize(size_t(f.size));
    in.seekg(std::streamoff(f.offset));
    return f.size == 0 || bool(in.read(out.data(), std::streamsize(f.size)));
}

bool Bundle::read(std::string_view path, std::string& out) const
{
    const File* f = find(path);
    return f && read(*f, out);
}

bool Bundle::extract(std::string_view folder, const fs::path& to, std::string& error) const
{
    std::ifstream in(file_, std::ios::binary);
    if (!in) {
        error = "Cannot read " + fromPath(file_);
        return false;
    }
    std::vector<char> buf(kChunk);
    std::error_code ec;
    for (const File& f : files_) {
        if (!under(f.path, folder))
            continue;
        const fs::path dst = to / toPath(std::string_view(f.path).substr(folder.empty() ? 0 : folder.size() + 1));
        fs::create_directories(dst.parent_path(), ec);
        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        if (!out || !copyRange(in, f.offset, f.size, out, nullptr, buf)) {
            error = "Cannot write " + fromPath(dst);
            return false;
        }
    }
    return true;
}

void addDirectory(std::vector<Source>& out, const fs::path& dir, const std::string& prefix)
{
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) {
            Source s;
            const std::string rel = fromPath(it->path().lexically_relative(dir));
            s.path = prefix.empty() ? rel : prefix + "/" + rel;
            s.disk = it->path();
            out.push_back(std::move(s));
        }
}

bool write(const fs::path& exe, const fs::path& out, const std::vector<Source>& sources, std::string& error)
{
    Bundle old;
    uint64_t programSize = 0;
    std::error_code ec;
    if (old.open(exe)) {
        programSize = old.programSize();
    } else {
        programSize = fs::file_size(exe, ec);
        if (ec) {
            error = "Cannot read " + fromPath(exe) + ": " + ec.message();
            return false;
        }
    }
    std::vector<const Source*> files;
    for (const Source& s : sources)
        files.push_back(&s);
    std::sort(files.begin(), files.end(), [](const Source* a, const Source* b) { return a->path < b->path; });
    for (size_t i = 0; i < files.size(); ++i) {
        const std::string& p = files[i]->path;
        if (p.empty() || p.size() > 0xFFFF || p.front() == '/' || (i > 0 && p == files[i - 1]->path)) {
            error = "Bad or repeated path in the bundle: " + p;
            return false;
        }
    }

    // (beside `out`, then moved over it: `out` may be `exe`, or in use)
    fs::path tmp = out;
    tmp += ".pack-tmp";
    {
        std::ifstream program(exe, std::ios::binary);
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!program || !o) {
            error = "Cannot write " + fromPath(tmp);
            return false;
        }
        std::vector<char> buf(kChunk);
        if (!copyRange(program, 0, programSize, o, nullptr, buf)) {
            error = "Cannot copy the program of " + fromPath(exe);
            fs::remove(tmp, ec);
            return false;
        }
        Hash id;
        std::string index;
        uint64_t at = programSize;
        for (const Source* s : files) {
            uint64_t size = 0;
            bool ok = false;
            Hash hash;
            if (s->from && s->entry) {
                std::ifstream in(s->from->file(), std::ios::binary);
                size = s->entry->size;
                ok = in && copyRange(in, s->entry->offset, size, o, &hash, buf);
            } else {
                std::ifstream in(s->disk, std::ios::binary | std::ios::ate);
                if (in) {
                    size = uint64_t(in.tellg());
                    ok = copyRange(in, 0, size, o, &hash, buf);
                }
            }
            if (!ok) {
                error = "Cannot pack " + (s->from ? s->path : fromPath(s->disk));
                o.close();
                fs::remove(tmp, ec);
                return false;
            }
            id.add(s->path.data(), s->path.size());
            id.add(&size, sizeof size);
            id.add(&hash.h, sizeof hash.h);
            index += char(uint8_t(s->path.size()));
            index += char(uint8_t(s->path.size() >> 8));
            index += s->path;
            putU64(index, at);
            putU64(index, size);
            putU64(index, hash.h);
            at += size;
        }
        if (!files.empty()) {
            std::string footer(kMagic, 8);
            putU32(footer, kVersion);
            putU32(footer, uint32_t(files.size()));
            putU64(footer, programSize);
            putU64(footer, at);
            putU64(footer, index.size());
            putU64(footer, id.h);
            o.write(index.data(), std::streamsize(index.size()));
            o.write(footer.data(), std::streamsize(footer.size()));
        }
        if (!o.flush()) {
            error = "Cannot write " + fromPath(tmp);
            o.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::permissions(tmp, fs::status(exe, ec).permissions(), ec);  // (executable on Linux and macOS)
    fs::rename(tmp, out, ec);
    if (ec) {
        error = "Cannot replace " + fromPath(out) + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace fx::bundle
