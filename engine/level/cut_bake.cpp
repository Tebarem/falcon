#include "level/cut_bake.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace fx::level {

namespace {

constexpr uint32_t kMagic = 0x54554346u;  // "FCUT"
constexpr uint32_t kVersion = 5;  // (3: damage cuts finer, their broken faces smooth; 4: no slivers across UV seams; 5: faces keep their materials)

struct Hash {
    uint64_t h = 0xcbf29ce484222325ull;
    void mix(const void* p, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
            h = (h ^ static_cast<const uint8_t*>(p)[i]) * 0x100000001b3ull;
    }
    template <typename T>
    void mix(const T& v)
    {
        mix(&v, sizeof(v));
    }
};

void put(std::vector<uint8_t>& out, const void* p, size_t n)
{
    out.insert(out.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
}

void put32(std::vector<uint8_t>& out, uint32_t v)
{
    put(out, &v, 4);
}

struct Reader {
    const uint8_t* at;
    const uint8_t* end;
    bool take(void* p, size_t n)
    {
        if (size_t(end - at) < n)
            return false;
        std::memcpy(p, at, n);
        at += n;
        return true;
    }
};

}  // namespace

uint64_t meshKey(const PolyMesh& mesh)
{
    const std::vector<uint8_t> blob = mesh.save();
    Hash h;
    h.mix(blob.data(), blob.size());
    return h.h;
}

uint64_t cutKey(uint64_t targetKey, const float world[3][4], const std::vector<Cut>& cuts)
{
    Hash h;
    h.mix(targetKey);
    h.mix(world, sizeof(float) * 12);
    for (const Cut& c : cuts) {
        h.mix(c.shape);
        h.mix(c.intersect);
        h.mix(c.world, sizeof(c.world));
        h.mix(c.size, sizeof(c.size));
        h.mix(c.seed);
        h.mix(c.roughness);
        h.mix(uint32_t(c.capMaterial.size()));
        h.mix(c.capMaterial.data(), c.capMaterial.size());
        h.mix(c.mesh ? meshKey(*c.mesh) : 0ull);
    }
    return h.h | 1u;
}

std::vector<uint8_t> CutBake::save() const
{
    std::vector<uint8_t> out;
    put32(out, kMagic);
    put32(out, kVersion);
    put32(out, uint32_t(results.size()));
    for (const auto& [key, r] : results) {
        put(out, &key, 8);
        const std::vector<uint8_t> blob = r->mesh->save();
        put32(out, uint32_t(blob.size()));
        put(out, blob.data(), blob.size());
        put32(out, uint32_t(r->sourceFace.size()));
        put(out, r->sourceFace.data(), r->sourceFace.size() * 4);
        put32(out, r->clipped ? 1u : 0u);  // (flags)
    }
    return out;
}

bool CutBake::load(const uint8_t* data, size_t size)
{
    results.clear();
    Reader r{data, data + size};
    uint32_t magic = 0, version = 0, count = 0;
    if (!r.take(&magic, 4) || !r.take(&version, 4) || !r.take(&count, 4) || magic != kMagic || version != kVersion)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t key = 0;
        uint32_t bytes = 0, faces = 0;
        if (!r.take(&key, 8) || !r.take(&bytes, 4) || size_t(r.end - r.at) < bytes) {
            results.clear();
            return false;
        }
        auto mesh = std::make_shared<PolyMesh>();
        if (!mesh->load(r.at, bytes)) {
            results.clear();
            return false;
        }
        r.at += bytes;
        auto baked = std::make_shared<BakedCut>();
        if (!r.take(&faces, 4) || faces != mesh->faces.size() || size_t(r.end - r.at) < size_t(faces) * 4) {
            results.clear();
            return false;
        }
        baked->sourceFace.resize(faces);
        r.take(baked->sourceFace.data(), size_t(faces) * 4);
        uint32_t flags = 0;
        if (!r.take(&flags, 4)) {
            results.clear();
            return false;
        }
        baked->clipped = (flags & 1u) != 0;
        baked->mesh = std::move(mesh);
        results[key] = std::move(baked);
    }
    return true;
}

bool CutBake::saveFile(const std::string& path) const
{
    if (results.empty()) {
        std::remove(path.c_str());
        return true;
    }
    const std::vector<uint8_t> blob = save();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    return f && f.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()));
}

bool CutBake::loadFile(const std::string& path)
{
    results.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return load(blob.data(), blob.size());
}

std::string cutBakePathFor(const std::string& scenePath)
{
    std::string p = scenePath;
    const size_t dot = p.find_last_of('.');
    const size_t slash = p.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        p.resize(dot);
    return p + ".cuts";
}

}  // namespace fx::level
