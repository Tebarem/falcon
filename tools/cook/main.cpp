// falcon cook: glTF -> .fpak (cookMain, run by falcon.exe's `cook` subcommand)
//   falcon cook <input.gltf|.glb> <output.fpak> [--force] [--verify] [--threads N] [--vt-max-size N]
//   falcon cook --empty <output.fpak> [--rocks] ...
//               [--graph graph.json]... [--library assets.gltf]... [--rocks]
// --library: the file's meshes are cooked too, with no instances of their own (assets for the
// terrain's scatter layers; the editor lists every mesh by name).
// --empty: no input, a scene with nothing placed (the built-ins, and the rocks with --rocks).
// --rocks: procedural rock meshes too (build_rocks.cpp: rock_boulder_a..c, rock_slab_a..b,
// rock_spire_a..b, rock_arch), for scatter.
// Writes <output>.fpak and its virtual texture pages <output>.fvt. Skips work when both
// outputs' source hash matches (inputs incl. images + cooker version + settings).
// --reuse other.fvt (repeatable; the output's own .fvt always counts): textures whose pages are
// in there (same images, size and alpha settings) are copied instead of encoded again.
// --progress file: "<stage> <done> <total> <reused>" rewritten a few times a second while
// cooking (stages: import, meshes, textures, writing, done; reused: textures copied from
// --reuse), for the editor and the launcher.

#include "cook.h"
#include "cook_main.h"
#include "assets/material_asset.h"

#include <box3d/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <mutex>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace fx;

namespace {

// Bump when cooked output changes for the same input.
constexpr uint64_t kCookerVersion = 17;  // 12: --rocks; 13: library metadata (mesh sources, node layouts); 14: --material;
                                         // 15: node animation, library cameras; 16: library skins and clips;
                                         // 17: MikkTSpace tangents for primitives without them

uint64_t hashBytes(uint64_t h, const void* data, size_t size)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    constexpr uint64_t kMul = 0x9E3779B97F4A7C15ull;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * kMul;
        h ^= h >> 32;
    }
    for (; i < size; ++i)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

bool hashFiles(const std::vector<std::string>& files, uint64_t& out)
{
    uint64_t h = hashBytes(0xcbf29ce484222325ull, &kCookerVersion, sizeof(kCookerVersion));
    h = hashBytes(h, &fpak::kVersion, sizeof(fpak::kVersion));
    std::vector<char> buf(1 << 20);
    for (const std::string& f : files) {
        std::ifstream in(f, std::ios::binary);
        if (!in)
            return false;
        while (in) {
            in.read(buf.data(), std::streamsize(buf.size()));
            h = hashBytes(h, buf.data(), size_t(in.gcount()));
        }
    }
    out = h;
    return true;
}

uint64_t alignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// --progress: the current stage and its count, written by a thread of its own.
class ProgressFile {
public:
    explicit ProgressFile(std::string path) : path_(std::move(path))
    {
        if (!path_.empty())
            thread_ = std::thread([this] {
                while (!stop_) {
                    write();
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                }
            });
    }
    ~ProgressFile() { finish("done"); }
    void stage(const char* name, const std::atomic<uint32_t>* done, uint32_t total, const std::atomic<uint32_t>* reused = nullptr)
    {
        std::lock_guard<std::mutex> lock(lock_);
        stage_ = name;
        done_ = done;
        total_ = total;
        if (reused)
            reused_ = reused;
    }
    // The last line, then no more.
    void finish(const char* name)
    {
        if (!thread_.joinable())
            return;
        stop_ = true;
        thread_.join();
        stage(name, nullptr, 0);
        write();
    }

private:
    void write()
    {
        char line[96];
        {
            std::lock_guard<std::mutex> lock(lock_);
            std::snprintf(line, sizeof(line), "%s %u %u %u\n", stage_, done_ ? done_->load() : 0u, total_,
                          reused_ ? reused_->load() : 0u);
        }
        if (FILE* f = std::fopen(path_.c_str(), "wb")) {
            std::fputs(line, f);
            std::fclose(f);
        }
    }

    std::string path_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex lock_;
    const char* stage_ = "import";
    const std::atomic<uint32_t>* done_ = nullptr;
    const std::atomic<uint32_t>* reused_ = nullptr;  // (kept through the later stages)
    uint32_t total_ = 0;
};

// Two skeletons are the same rig when their bones, parents and bind poses match (an
// animations-only export of a model shares the model's skeleton).
bool sameRig(const cook::SrcSkeleton& a, const cook::SrcSkeleton& b)
{
    if (a.boneNames != b.boneNames || a.parents != b.parents || a.inverseBind.size() != b.inverseBind.size())
        return false;
    for (size_t i = 0; i < a.inverseBind.size(); ++i)
        for (int k = 0; k < 16; ++k)
            if (std::fabs(a.inverseBind[i][k] - b.inverseBind[i][k]) > 1e-4f)
                return false;
    return true;
}

// A library's meshes, materials, images, skeletons and clips appended to the scene (its
// instances name its meshes and are kept as the library's layout, not placed). Its skeletons are
// named "<source>#<skin>" (a graph's "skeleton": "<source>" means the first), or reuse a
// skeleton already in the scene with the same rig; a clip keeps its name unless the skeleton
// already has one of that name, then it is "<source>#<clip>". Returns the library's first primitive.
uint32_t mergeLibrary(cook::SrcScene& scene, cook::SrcScene& lib, const std::string& source, std::vector<std::string>& primNames)
{
    const int imageBase = int(scene.images.size());
    const uint32_t materialBase = uint32_t(scene.materials.size()), primBase = uint32_t(scene.primitives.size());
    for (cook::SrcImage& img : lib.images)
        scene.images.push_back(std::move(img));
    for (size_t m = 0; m < lib.materials.size(); ++m) {
        scene.materials.push_back(lib.materials[m]);
        cook::SrcMaterialTextures t = lib.materialTextures[m];
        for (int* i : {&t.baseColor, &t.normal, &t.metallicRoughness, &t.occlusion})
            if (*i >= 0)
                *i += imageBase;
        scene.materialTextures.push_back(t);
    }
    std::vector<int32_t> skeletonOf(lib.skeletons.size());
    for (size_t s = 0; s < lib.skeletons.size(); ++s) {
        skeletonOf[s] = -1;
        for (size_t o = 0; o < scene.skeletons.size() && skeletonOf[s] < 0; ++o)
            if (sameRig(scene.skeletons[o], lib.skeletons[s]))
                skeletonOf[s] = int32_t(o);
        if (skeletonOf[s] < 0) {
            skeletonOf[s] = int32_t(scene.skeletons.size());
            lib.skeletons[s].name = source + "#" + lib.skeletons[s].name;
            scene.skeletons.push_back(std::move(lib.skeletons[s]));
        }
    }
    for (cook::SrcClip& c : lib.clips) {
        c.skeleton = uint32_t(skeletonOf[c.skeleton]);
        for (const cook::SrcClip& o : scene.clips)
            if (o.skeleton == c.skeleton && o.name == c.name) {
                c.name = source + "#" + c.name;
                break;
            }
        scene.clips.push_back(std::move(c));
    }
    for (cook::SrcPrimitive& p : lib.primitives) {
        p.material += materialBase;
        if (p.skeleton >= 0)
            p.skeleton = skeletonOf[p.skeleton];
        scene.primitives.push_back(std::move(p));
    }
    primNames.resize(scene.primitives.size());
    for (const cook::SrcInstance& inst : lib.instances)
        if (primNames[primBase + inst.primitive].empty())
            primNames[primBase + inst.primitive] = inst.name;
    return primBase;
}

// JSON text, escaped.
std::string jsonString(const std::string& s)
{
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        if (uint8_t(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(uint8_t(c)));
            out += buf;
            continue;
        }
        out += c;
    }
    return out + '"';
}

struct SectionData {
    const void* data;
    uint64_t size;
    uint32_t count, stride;
};

template <typename T>
SectionData section(const std::vector<T>& v)
{
    return {v.data(), v.size() * sizeof(T), uint32_t(v.size()), uint32_t(sizeof(T))};
}

}  // namespace

int cookMain(int argc, char** argv)
{
    std::string input, output;
    bool force = false, verify = false;
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    uint32_t vtMaxSize = 4096;
    std::vector<std::string> graphPaths, libraryPaths, materialPaths, reusePaths;
    std::vector<std::pair<std::string, std::string>> displaced;  // --displace mesh=material (its --material path)
    std::string sourceRoot;  // library paths are recorded relative to it (a project's folder)
    std::string progressPath;
    bool rocks = false, empty = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--force")) force = true;
        else if (!std::strcmp(argv[i], "--verify")) verify = true;
        else if (!std::strcmp(argv[i], "--threads") && i + 1 < argc) threads = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--vt-max-size") && i + 1 < argc) vtMaxSize = uint32_t(std::max(128, std::atoi(argv[++i])));
        else if (!std::strcmp(argv[i], "--graph") && i + 1 < argc) graphPaths.push_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--library") && i + 1 < argc) libraryPaths.push_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--source-root") && i + 1 < argc) sourceRoot = argv[++i];
        else if (!std::strcmp(argv[i], "--material") && i + 1 < argc) materialPaths.push_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--displace") && i + 1 < argc) {
            const std::string pair = argv[++i];
            const size_t eq = pair.find('=');
            if (eq == std::string::npos) {
                std::fprintf(stderr, "falcon cook: --displace takes mesh=material.fmat\n");
                return 2;
            }
            displaced.push_back({pair.substr(0, eq), pair.substr(eq + 1)});
        }
        else if (!std::strcmp(argv[i], "--reuse") && i + 1 < argc) reusePaths.push_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--progress") && i + 1 < argc) progressPath = argv[++i];
        else if (!std::strcmp(argv[i], "--rocks")) rocks = true;
        else if (!std::strcmp(argv[i], "--empty")) empty = true;
        else if (input.empty()) input = argv[i];
        else if (output.empty()) output = argv[i];
        else {
            std::fprintf(stderr, "falcon cook: unexpected argument %s\n", argv[i]);
            return 2;
        }
    }
    if (empty && output.empty()) {  // no input: the one path is the output
        output = input;
        input = "(empty scene)";
    }
    if (input.empty() || output.empty()) {
        std::fprintf(stderr, "usage: falcon cook <input.gltf|.glb> <output.fpak> | --empty <output.fpak>  [--force] [--verify] [--threads N] [--vt-max-size N] [--graph graph.json]... [--library assets.gltf]... [--material m.fmat]... [--displace mesh=m.fmat]... [--source-root dir] [--rocks] [--reuse pages.fvt]... [--progress file]\n");
        return 2;
    }
    b3SetAssertFcn([](const char* condition, const char* file, int line) {
        std::fprintf(stderr, "falcon cook: Box3D assert %s (%s:%d)\n", condition, file, line);
        std::fflush(stderr);
        return 1;
    });
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };

    // The counters outlive `progress`, which reads them.
    cook::VtProgress vtProgress;
    std::atomic<uint32_t> imported{0}, meshesBuilt{0};
    ProgressFile progress(progressPath);
    progress.stage("import", &imported, uint32_t(libraryPaths.size()));

    cook::SrcScene scene;
    std::vector<std::string> sources;
    if (empty) {
        // Nothing but the default material (what the importer starts every scene with).
        fpak::MaterialGpu m = {};
        m.baseColor[0] = m.baseColor[1] = m.baseColor[2] = m.baseColor[3] = 1.0f;
        m.roughness = 1.0f;
        m.alphaCutoff = 0.5f;
        m.vtPageTable = fpak::kVtNoTexture;
        m.normalScale = 1.0f;
        m.occlusionStrength = 1.0f;
        scene.materials.push_back(m);
        scene.materialTextures.push_back({});
    } else if (!cook::importGltf(input, scene, sources)) {
        return 1;
    }
    // Mesh names: the first instance that uses each one.
    std::vector<std::string> primNames(scene.primitives.size());
    for (const cook::SrcInstance& inst : scene.instances)
        if (primNames[inst.primitive].empty())
            primNames[inst.primitive] = inst.name;
    // Each library's source (relative to --source-root) and its nodes, kept for the editor: a
    // model placed whole, its preview, which model a mesh came from.
    struct Library {
        std::string source;
        std::vector<cook::SrcInstance> nodes;  // primitives: scene-wide
        std::vector<cook::SrcCamera> cameras;
        std::vector<cook::SrcNodeTrack> tracks;  // its rigid animation (nodes: its node indices)
        std::vector<std::vector<float>> trackData;
        uint32_t frames = 0;
        float fps = 30.0f;
    };
    std::vector<Library> libraries;
    for (const std::string& lib : libraryPaths) {
        cook::SrcScene libScene;
        if (!cook::importGltf(lib, libScene, sources))
            return 1;
        Library info;
        std::error_code ec;
        info.source = sourceRoot.empty() ? fs::path(lib).generic_string()
                                         : fs::path(lib).lexically_relative(sourceRoot).generic_string();
        info.nodes = libScene.instances;
        info.cameras = libScene.cameras;
        info.tracks = std::move(libScene.nodeTracks);
        info.trackData = std::move(libScene.nodeTrackData);
        info.frames = libScene.nodeFrames;
        info.fps = libScene.nodeFps;
        const uint32_t base = mergeLibrary(scene, libScene, info.source, primNames);
        for (cook::SrcInstance& n : info.nodes)
            n.primitive += base;
        libraries.push_back(std::move(info));
        ++imported;
    }
    // Material assets (.fmat): a material each, its textures (paths relative to --source-root)
    // into the virtual texture like a glTF's.
    std::vector<std::pair<std::string, uint32_t>> materialAssets;  // source, material index
    std::vector<MaterialAsset> materialAssetData;                  // parallel: displacement
    for (const std::string& path : materialPaths) {
        MaterialAsset asset;
        std::string error;
        if (!readMaterialAsset(path, asset, error)) {
            std::fprintf(stderr, "falcon cook: %s\n", error.c_str());
            return 1;
        }
        fpak::MaterialGpu m = {};
        m.vtPageTable = fpak::kVtNoTexture;
        applyMaterialFactors(asset, m);
        cook::SrcMaterialTextures textures;
        int* slots[4] = {&textures.baseColor, &textures.normal, &textures.metallicRoughness, &textures.occlusion};
        for (int s = 0; s < 4; ++s) {
            if (asset.textures[s].empty())
                continue;
            const fs::path image = sourceRoot.empty() ? fs::path(asset.textures[s]) : fs::path(sourceRoot) / asset.textures[s];
            std::error_code ec;
            if (!fs::exists(image, ec)) {
                std::fprintf(stderr, "falcon cook: %s: no texture %s (left out)\n", path.c_str(), image.string().c_str());
                continue;
            }
            cook::SrcImage img;
            img.path = image.string();
            *slots[s] = int(scene.images.size());
            scene.images.push_back(std::move(img));
            sources.push_back(image.string());
        }
        // The height map: the ORM pages' alpha (decals' parallax).
        if (!asset.height.empty()) {
            const fs::path image = sourceRoot.empty() ? fs::path(asset.height) : fs::path(sourceRoot) / asset.height;
            std::error_code ec;
            if (fs::exists(image, ec)) {
                cook::SrcImage img;
                img.path = image.string();
                textures.height = int(scene.images.size());
                scene.images.push_back(std::move(img));
            }
        }
        sources.push_back(path);
        if (asset.displaced()) {  // (the height map: hashed, read when a mesh is displaced with it)
            const fs::path image = sourceRoot.empty() ? fs::path(asset.height) : fs::path(sourceRoot) / asset.height;
            std::error_code ec;
            if (fs::exists(image, ec))
                sources.push_back(image.string());
        }
        materialAssetData.push_back(asset);
        materialAssets.push_back({sourceRoot.empty() ? fs::path(path).generic_string() : fs::path(path).lexically_relative(sourceRoot).generic_string(),
                                  uint32_t(scene.materials.size())});
        scene.materials.push_back(m);
        scene.materialTextures.push_back(textures);
    }
    sources.insert(sources.end(), graphPaths.begin(), graphPaths.end());
    uint64_t sourceHash = 0;
    if (!hashFiles(sources, sourceHash)) {
        std::fprintf(stderr, "falcon cook: cannot read sources for hashing\n");
        return 1;
    }
    sourceHash = hashBytes(sourceHash, &vtMaxSize, sizeof(vtMaxSize));
    sourceHash = hashBytes(sourceHash, &rocks, sizeof(rocks));
    sourceHash = hashBytes(sourceHash, &empty, sizeof(empty));
    for (const auto& [mesh, material] : displaced) {
        sourceHash = hashBytes(sourceHash, mesh.data(), mesh.size());
        sourceHash = hashBytes(sourceHash, material.data(), material.size());
    }
    sourceHash = hashBytes(sourceHash, sourceRoot.data(), sourceRoot.size());
    const std::string vtOutput = fs::path(output).replace_extension(".fvt").string();
    if (!force && !verify) {
        std::ifstream existing(output, std::ios::binary), existingVt(vtOutput, std::ios::binary);
        fpak::Header h = {};
        fpak::VtFileHeader vh = {};
        if (existing.read(reinterpret_cast<char*>(&h), sizeof(h)) && !std::memcmp(h.magic, fpak::kMagic, 4) &&
            h.version == fpak::kVersion && h.sourceHash == sourceHash &&
            existingVt.read(reinterpret_cast<char*>(&vh), sizeof(vh)) && vh.sourceHash == sourceHash) {
            std::printf("falcon cook: %s is up to date\n", output.c_str());
            return 0;
        }
    }
    if (rocks)
        cook::appendRockPrimitives(scene, primNames);
    uint32_t builtins[uint32_t(fpak::Builtin::Count)] = {}, builtinMaterial = 0;
    cook::appendBuiltinPrimitives(scene, builtins, builtinMaterial);
    primNames.resize(scene.primitives.size());
    for (uint32_t k = 0; k < uint32_t(fpak::Builtin::Count); ++k)
        primNames[builtins[k]] = fpak::kBuiltinNames[k];
    std::vector<std::pair<uint32_t, uint32_t>> displacedFrom;  // displaced mesh, its source mesh
    // Displaced meshes: a copy of the mesh, subdivided and pushed out by the material's height
    // map, drawn with that material, named "<mesh>~<material source>" (the editor's entities with
    // that mesh and material load it instead).
    for (const auto& [meshName, materialPath] : displaced) {
        size_t k = 0;
        while (k < materialPaths.size() && materialPaths[k] != materialPath)
            ++k;
        const auto prim = std::find(primNames.begin(), primNames.end(), meshName);
        if (k == materialPaths.size() || prim == primNames.end()) {
            std::fprintf(stderr, "falcon cook: --displace %s=%s: no such %s (left out)\n", meshName.c_str(), materialPath.c_str(),
                         k == materialPaths.size() ? "--material" : "mesh");
            continue;
        }
        const MaterialAsset& asset = materialAssetData[k];
        const uint32_t p = uint32_t(prim - primNames.begin());
        if (!asset.displaced() || scene.primitives[p].skeleton >= 0) {
            std::fprintf(stderr, "falcon cook: --displace %s=%s: %s (left out)\n", meshName.c_str(), materialPath.c_str(),
                         asset.displaced() ? "skinned meshes are not displaced" : "the material has no height displacement");
            continue;
        }
        cook::HeightMap map;
        const fs::path image = sourceRoot.empty() ? fs::path(asset.height) : fs::path(sourceRoot) / asset.height;
        if (!cook::loadHeightMap(image.string(), map)) {
            std::fprintf(stderr, "falcon cook: %s: cannot read the height map %s (left out)\n", materialPath.c_str(), image.string().c_str());
            continue;
        }
        std::string note;
        cook::SrcPrimitive d = cook::displacePrimitive(scene.primitives[p], map, {asset.displacement, asset.heightCenter, asset.displacementDetail}, note);
        d.material = materialAssets[k].second;
        std::printf("falcon cook: %s displaced by %s: %zu -> %zu triangles%s%s\n", meshName.c_str(), materialAssets[k].first.c_str(),
                    scene.primitives[p].indices.size() / 3, d.indices.size() / 3, note.empty() ? "" : ", ", note.c_str());
        displacedFrom.push_back({uint32_t(scene.primitives.size()), p});
        scene.primitives.push_back(std::move(d));
        primNames.push_back(meshName + "~" + materialAssets[k].first);
    }
    const auto builtinKind = [&](uint32_t p) {
        for (uint32_t k = 0; k < uint32_t(fpak::Builtin::Count); ++k)
            if (builtins[k] == p)
                return int(k);
        return -1;
    };
    const double importTime = elapsed();

    // Build meshes in parallel, largest first; results land by index (deterministic output).
    std::vector<cook::BuiltMesh> built(scene.primitives.size());
    std::vector<cook::BuiltCollider> colliders(scene.primitives.size());
    std::vector<uint32_t> order(scene.primitives.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return scene.primitives[a].indices.size() > scene.primitives[b].indices.size();
    });
    std::atomic<size_t> next{0};
    progress.stage("meshes", &meshesBuilt, uint32_t(order.size()));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::min<size_t>(threads, order.size()); ++t)
        pool.emplace_back([&] {
            for (size_t i; (i = next.fetch_add(1)) < order.size();) {
                const uint32_t p = order[i];
                const cook::SrcPrimitive& prim = scene.primitives[p];
                if (prim.indices.empty())
                    continue;
                const uint32_t flags = scene.materials[prim.material].flags;
                const bool foliage = (flags & fpak::kMaterialAlphaMask) && (flags & fpak::kMaterialDoubleSided);
                built[p] = cook::buildMesh(prim, foliage);
                // Built-in primitives get no static triangle mesh; the cube and sphere (dynamic
                // bodies) get a hull. Skinned meshes deform, so they get no collision at all.
                const int kind = builtinKind(p);
                const bool copy = std::any_of(displacedFrom.begin(), displacedFrom.end(), [&](const auto& d) { return d.first == p; });
                if (prim.skeleton < 0 && !copy && (kind < 0 || kind == int(fpak::Builtin::Cube) || kind == int(fpak::Builtin::Sphere)))
                    colliders[p] = cook::buildCollider(prim, scene.materials[prim.material], kind >= 0);
                ++meshesBuilt;
            }
        });
    for (auto& t : pool)
        t.join();
    for (const auto& [mesh, from] : displacedFrom)  // (a displaced mesh collides like its source: not a million triangles)
        colliders[mesh] = colliders[from];
    const double buildTime = elapsed();

    // Virtual textures: layout first (page-table offsets go into the materials), then encode.
    uint32_t vtPages = 0;
    const std::vector<cook::VtLayout> vtLayouts = cook::layoutVirtualTextures(scene, vtMaxSize, vtPages);
    uint32_t texturedMaterials = 0;
    for (size_t m = 0; m < scene.materials.size(); ++m) {
        const cook::VtLayout& l = vtLayouts[m];
        if (!l.pageCount)
            continue;
        scene.materials[m].vtPageTable = l.firstPage;
        scene.materials[m].vtSize = (scene.materials[m].vtSize & fpak::kVtRoughnessMinMask) | l.log2Width | l.log2Height << 8 | l.mipCount << 16;
        ++texturedMaterials;
    }
    const fs::path vtTmp = vtOutput + ".tmp";
    std::error_code existsError;
    if (fs::exists(vtOutput, existsError))
        reusePaths.insert(reusePaths.begin(), vtOutput);
    vtProgress.total = texturedMaterials;
    progress.stage("textures", &vtProgress.done, texturedMaterials, &vtProgress.reused);
    if (!cook::encodeVirtualTextures(scene, vtLayouts, vtPages, sourceHash, vtTmp.string(), threads, reusePaths, vtProgress)) {
        std::fprintf(stderr, "falcon cook: virtual texture encoding failed (is the disk full?)\n");
        return 1;
    }
    const double textureTime = elapsed();
    progress.stage("writing", nullptr, 0);

    size_t violations = 0;
    if (verify)
        for (uint32_t m = 0; m < built.size(); ++m)
            violations += cook::verifyMesh(scene.primitives[m], built[m], m);

    // Concatenate meshes, rebasing local offsets into the global buffers.
    std::vector<fpak::VertexGpu> vertices;
    std::vector<uint32_t> clusterVertices, clusterTriangles;
    std::vector<fpak::ClusterGpu> clusters;
    std::vector<fpak::LodNodeGpu> lodNodes;
    std::vector<fpak::MeshGpu> meshes;
    std::vector<fpak::SkinVertexGpu> skinVertices;
    std::vector<fpak::MeshSkinGpu> meshSkins;
    uint32_t totalTriangles = 0, maxLevels = 0;
    for (size_t bi = 0; bi < built.size(); ++bi) {
        const cook::BuiltMesh& b = built[bi];
        fpak::MeshSkinGpu skin = {uint32_t(vertices.size()), uint32_t(b.vertices.size()), fpak::kNotSkinned, 0};
        if (!b.skin.empty()) {
            skin.firstSkinVertex = uint32_t(skinVertices.size());
            skin.skeleton = uint32_t(scene.primitives[bi].skeleton);
            skinVertices.insert(skinVertices.end(), b.skin.begin(), b.skin.end());
        }
        meshSkins.push_back(skin);
        const uint32_t vBase = uint32_t(vertices.size()), cvBase = uint32_t(clusterVertices.size()),
                       ctBase = uint32_t(clusterTriangles.size()), cBase = uint32_t(clusters.size()),
                       nBase = uint32_t(lodNodes.size());
        vertices.insert(vertices.end(), b.vertices.begin(), b.vertices.end());
        for (uint32_t v : b.clusterVertices)
            clusterVertices.push_back(v + vBase);
        clusterTriangles.insert(clusterTriangles.end(), b.clusterTriangles.begin(), b.clusterTriangles.end());
        for (fpak::ClusterGpu c : b.clusters) {
            c.vertexOffset += cvBase;
            c.triangleOffset += ctBase;
            clusters.push_back(c);
        }
        for (fpak::LodNodeGpu n : b.lodNodes) {
            n.childOffset += n.group >= 0 ? cBase : nBase;
            lodNodes.push_back(n);
        }
        fpak::MeshGpu mesh = b.mesh;
        mesh.clusterOffset = cBase;
        mesh.lodNodeOffset = nBase;
        meshes.push_back(mesh);
        totalTriangles += b.totalTriangles;
        maxLevels = std::max(maxLevels, b.levels);
    }

    std::vector<fpak::InstanceGpu> instances;
    float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    uint64_t leafTriangles = 0;
    for (const cook::SrcInstance& si : scene.instances) {
        fpak::InstanceGpu g = {};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                g.world[r][c] = si.world[c * 4 + r];
        g.mesh = si.primitive;
        instances.push_back(g);
        const cook::BuiltMesh& b = built[si.primitive];
        leafTriangles += b.leafTriangles;
        for (int corner = 0; corner < 8; ++corner) {
            float p[3];
            for (int a = 0; a < 3; ++a)
                p[a] = (corner >> a) & 1 ? b.surfaceMax[a] : b.surfaceMin[a];
            for (int r = 0; r < 3; ++r) {
                const float w = g.world[r][0] * p[0] + g.world[r][1] * p[1] + g.world[r][2] * p[2] + g.world[r][3];
                lo[r] = std::min(lo[r], w);
                hi[r] = std::max(hi[r], w);
            }
        }
    }

    if (scene.instances.empty()) {  // nothing placed (an empty scene): a 100 x 20 x 100 m box at the origin
        const float boxLo[3] = {-50.0f, -1.0f, -50.0f}, boxHi[3] = {50.0f, 19.0f, 50.0f};
        std::copy(boxLo, boxLo + 3, lo);
        std::copy(boxHi, boxHi + 3, hi);
    }

    // Instance names: an offset table, then the strings (the editor outliner reads them).
    std::vector<uint8_t> names(scene.instances.size() * sizeof(uint32_t));
    for (size_t i = 0; i < scene.instances.size(); ++i) {
        const uint32_t at = uint32_t(names.size());
        std::memcpy(names.data() + i * sizeof(uint32_t), &at, sizeof(at));
        const std::string& n = scene.instances[i].name;
        names.insert(names.end(), n.begin(), n.end());
        names.push_back(0);
    }
    // Then the meshes' names (unique: repeats get a number), as a trailer old readers ignore:
    // strings, an offset per mesh, the count, "MSHN".
    {
        std::vector<uint32_t> at(primNames.size());
        std::map<std::string, uint32_t> seen;
        for (size_t m = 0; m < primNames.size(); ++m) {
            std::string n = primNames[m].empty() ? "Mesh " + std::to_string(m) : primNames[m];
            if (const uint32_t count = seen[n]++; count)
                n += " (" + std::to_string(count + 1) + ")";
            at[m] = uint32_t(names.size());
            names.insert(names.end(), n.begin(), n.end());
            names.push_back(0);
        }
        names.resize((names.size() + 3) & ~size_t(3));
        // Library metadata (JSON), just before the offsets: text, its size, "FXMD". Readers that
        // only know "MSHN" never look there.
        if (!libraries.empty() || !materialAssets.empty()) {
            std::string meta = "{\"materials\":[";
            for (size_t k = 0; k < materialAssets.size(); ++k)
                meta += (k ? ",{" : "{") + std::string("\"source\":") + jsonString(materialAssets[k].first) +
                        ",\"index\":" + std::to_string(materialAssets[k].second) + "}";
            meta += "],\"libraries\":[";
            for (size_t l = 0; l < libraries.size(); ++l) {
                meta += (l ? ",{" : "{") + std::string("\"source\":") + jsonString(libraries[l].source) + ",\"nodes\":[";
                for (size_t n = 0; n < libraries[l].nodes.size(); ++n) {
                    const cook::SrcInstance& node = libraries[l].nodes[n];
                    char buf[64];
                    meta += (n ? ",{" : "{") + std::string("\"mesh\":") + std::to_string(node.primitive) + ",\"name\":" + jsonString(node.name) + ",\"world\":[";
                    for (int r = 0; r < 3; ++r)  // rows of the 3x4 world matrix (glTF's is column-major)
                        for (int c = 0; c < 4; ++c) {
                            std::snprintf(buf, sizeof(buf), "%s%.9g", r || c ? "," : "", double(node.world[c * 4 + r]));
                            meta += buf;
                        }
                    meta += "]}";
                }
                meta += "],\"cameras\":[";
                for (size_t c = 0; c < libraries[l].cameras.size(); ++c) {
                    const cook::SrcCamera& cam = libraries[l].cameras[c];
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%.9g", double(cam.yfov));
                    meta += (c ? ",{" : "{") + std::string("\"name\":") + jsonString(cam.name) + ",\"yfov\":" + buf + ",\"world\":[";
                    for (int r = 0; r < 3; ++r)
                        for (int col = 0; col < 4; ++col) {
                            std::snprintf(buf, sizeof(buf), "%s%.9g", r || col ? "," : "", double(cam.world[col * 4 + r]));
                            meta += buf;
                        }
                    meta += "]}";
                }
                meta += "]}";
            }
            meta += "]}";
            names.insert(names.end(), meta.begin(), meta.end());
            names.resize((names.size() + 3) & ~size_t(3));
            const uint32_t metaSize = uint32_t(meta.size());
            const size_t m = names.size();
            names.resize(m + 8);
            std::memcpy(names.data() + m, &metaSize, 4);
            std::memcpy(names.data() + m + 4, "FXMD", 4);
        }
        const uint32_t count = uint32_t(at.size());
        const size_t base = names.size();
        names.resize(base + at.size() * 4 + 8);
        std::memcpy(names.data() + base, at.data(), at.size() * 4);
        std::memcpy(names.data() + base + at.size() * 4, &count, 4);
        std::memcpy(names.data() + base + at.size() * 4 + 4, "MSHN", 4);
    }

    std::vector<cook::CompiledGraph> graphs(graphPaths.size());
    for (size_t g = 0; g < graphPaths.size(); ++g)
        if (!cook::compileGraph(scene, graphPaths[g], graphs[g]))
            return 1;
    const std::vector<uint8_t> animation = cook::buildAnimation(scene, graphs);
    if (verify)
        violations += cook::verifyAnimation(scene, animation);

    // Collision blobs, 16-byte aligned (Box3D reads them in place).
    std::vector<uint8_t> colliderBytes;
    std::vector<fpak::MeshCollider> meshColliders(colliders.size());
    uint64_t collisionTriangles = 0;
    uint32_t hulls = 0;
    auto appendBlob = [&](const std::vector<uint8_t>& blob, uint32_t& offset, uint32_t& bytes) {
        if (blob.empty())
            return;
        colliderBytes.resize((colliderBytes.size() + 15) & ~size_t(15));
        offset = uint32_t(colliderBytes.size());
        bytes = uint32_t(blob.size());
        colliderBytes.insert(colliderBytes.end(), blob.begin(), blob.end());
    };
    for (size_t m = 0; m < colliders.size(); ++m) {
        appendBlob(colliders[m].mesh, meshColliders[m].meshOffset, meshColliders[m].meshBytes);
        appendBlob(colliders[m].hull, meshColliders[m].hullOffset, meshColliders[m].hullBytes);
        collisionTriangles += colliders[m].meshTriangles;
        hulls += colliders[m].hull.empty() ? 0 : 1;
    }

    // Rigid node animation: every library's tracks (by library, node or camera) and the scene's
    // own (by cooked instance), then their rows; a node's primitives share one copy of its rows.
    std::vector<uint8_t> nodeAnimation;
    uint32_t nodeTracks = 0;
    {
        std::vector<fpak::NodeTrackEntry> entries;
        std::vector<float> rows;
        float fps = scene.nodeFps;
        const auto add = [&](uint32_t library, const std::vector<cook::SrcNodeTrack>& tracks, const std::vector<std::vector<float>>& data,
                             uint32_t loop) {
            std::vector<uint32_t> offsets(data.size(), ~0u);
            for (const cook::SrcNodeTrack& t : tracks) {
                if (offsets[t.data] == ~0u) {
                    offsets[t.data] = uint32_t(rows.size());
                    rows.insert(rows.end(), data[t.data].begin(), data[t.data].end());
                }
                entries.push_back({library, t.target, t.camera ? fpak::kNodeTrackCamera : 0u, t.first, t.count, loop, offsets[t.data], 0});
            }
        };
        add(fpak::kNodeAnimScene, scene.nodeTracks, scene.nodeTrackData, scene.nodeFrames);
        for (uint32_t l = 0; l < libraries.size(); ++l) {
            add(l, libraries[l].tracks, libraries[l].trackData, libraries[l].frames);
            if (!libraries[l].tracks.empty())
                fps = libraries[l].fps;
        }
        nodeTracks = uint32_t(entries.size());
        if (!entries.empty()) {
            const fpak::NodeAnimHeader h = {nodeTracks, fps, uint32_t(rows.size()), 0};
            const auto append = [&](const void* p, size_t n) {
                nodeAnimation.insert(nodeAnimation.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
            };
            append(&h, sizeof(h));
            append(entries.data(), entries.size() * sizeof(fpak::NodeTrackEntry));
            append(rows.data(), rows.size() * sizeof(float));
        }
    }

    // Layout: header, then page-aligned sections in enum order.
    fpak::Header header = {};
    std::memcpy(header.magic, fpak::kMagic, 4);
    header.version = fpak::kVersion;
    header.sourceHash = sourceHash;
    std::memcpy(header.boundsMin, lo, sizeof(lo));
    std::memcpy(header.boundsMax, hi, sizeof(hi));
    header.leafTriangles = uint32_t(std::min<uint64_t>(leafTriangles, UINT32_MAX));
    header.totalTriangles = totalTriangles;
    header.vtPageCount = vtPages;
    header.vtPageBytes = fpak::kVtPageBytes;
    std::memcpy(header.builtinMeshes, builtins, sizeof(builtins));
    header.builtinMaterial = builtinMaterial;
    const SectionData sections[] = {section(vertices),  section(clusterVertices), section(clusterTriangles),
                                    section(clusters),  section(lodNodes),        section(meshes),
                                    section(instances), section(scene.materials), section(scene.lights),
                                    section(skinVertices), section(meshSkins),
                                    section(colliderBytes), section(meshColliders), section(animation),
                                    section(names), section(nodeAnimation)};
    static_assert(std::size(sections) == size_t(fpak::Section::Count));
    uint64_t offset = alignUp(sizeof(header), fpak::kSectionAlign);
    for (size_t s = 0; s < std::size(sections); ++s) {
        header.sections[s] = {offset, sections[s].size, sections[s].count, sections[s].stride};
        offset = alignUp(offset + sections[s].size, fpak::kSectionAlign);
    }

    const fs::path tmp = output + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "falcon cook: cannot write %s\n", tmp.string().c_str());
            return 1;
        }
        std::vector<char> zeros(fpak::kSectionAlign, 0);
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        uint64_t pos = sizeof(header);
        for (size_t s = 0; s < std::size(sections); ++s) {
            out.write(zeros.data(), std::streamsize(header.sections[s].offset - pos));
            out.write(static_cast<const char*>(sections[s].data), std::streamsize(sections[s].size));
            pos = header.sections[s].offset + sections[s].size;
        }
        out.write(zeros.data(), std::streamsize(offset - pos));
        if (!out) {
            std::fprintf(stderr, "falcon cook: write failed\n");
            return 1;
        }
    }
    std::error_code ec;
    fs::rename(vtTmp, vtOutput, ec);
    if (!ec)
        fs::rename(tmp, output, ec);
    if (ec) {
        std::fprintf(stderr, "falcon cook: cannot replace %s: %s\n", output.c_str(), ec.message().c_str());
        return 1;
    }

    std::printf("falcon cook: %s -> %s\n", input.c_str(), output.c_str());
    std::printf("  %zu meshes, %zu instances, %zu materials, %zu clusters, %zu lod nodes, %u DAG levels\n",
                meshes.size(), instances.size(), scene.materials.size(), clusters.size(), lodNodes.size(), maxLevels);
    std::printf("  %llu leaf triangles (instanced), %u triangles in all LODs, %zu vertices\n",
                (unsigned long long)leafTriangles, totalTriangles, vertices.size());
    std::printf("  %zu lights\n", scene.lights.size());
    std::printf("  animation: %zu skeletons, %zu clips, %zu graphs, %zu skinned vertices, %.2f MB\n", scene.skeletons.size(),
                scene.clips.size(), graphs.size(), skinVertices.size(), double(animation.size()) / (1 << 20));
    if (nodeTracks)
        std::printf("  node animation: %u tracks, %.1f MB\n", nodeTracks, double(nodeAnimation.size()) / (1 << 20));
    std::printf("  collision: %llu static triangles, %u hulls, %.1f MB\n", (unsigned long long)collisionTriangles, hulls,
                double(colliderBytes.size()) / (1 << 20));
    std::printf("  virtual textures: %u materials (%u reused), %u pages, %.1f MB (max %u texels)\n", texturedMaterials,
                vtProgress.reused.load(), vtPages, double(vtPages) * fpak::kVtPageBytes / (1 << 20), vtMaxSize);
    std::printf("  %.1f MB, import %.2fs, meshes %.2fs, textures %.2fs, total %.2fs (%u threads)\n",
                double(offset) / (1 << 20), importTime, buildTime - importTime, textureTime - buildTime, elapsed(),
                threads);
    if (verify) {
        std::printf("  verify: %zu violations\n", violations);
        if (violations)
            return 1;
    }
    return 0;
}
