#pragma once

#include "assets/fpak_format.h"

#include <SDL3/SDL_asyncio.h>
#include <SDL3/SDL_gpu.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

struct b3MeshData;
struct b3HullData;

namespace fx {

// A cooked scene package. Loading is split so file I/O overlaps GPU device creation:
//   beginRead()  at the top of main (async, no device needed)
//   finishRead() when the CPU data is needed (blocks only if I/O is still running)
//   upload()     once the device exists: one transfer buffer, one copy pass, no waits
class ScenePak {
public:
    bool beginRead(const char* path);
    // Whether the read is done (finishRead won't wait), without waiting: Scene.LoadAsync.
    bool readDone();
    bool finishRead();
    bool upload(SDL_GPUDevice* device);
    void release(SDL_GPUDevice* device);

    const fpak::Header& header() const { return *reinterpret_cast<const fpak::Header*>(data_); }
    // The path it reads (borrowed: the caller keeps the string alive), and a new home for it.
    const char* path() const { return path_ ? path_ : ""; }
    void setPath(const char* path) { path_ = path; }
    size_t fileSize() const { return size_; }

    template <typename T>
    const T* view(fpak::Section s, uint32_t* count = nullptr) const
    {
        const fpak::SectionEntry& e = header().sections[uint32_t(s)];
        if (count)
            *count = e.count;
        return reinterpret_cast<const T*>(static_cast<const uint8_t*>(data_) + e.offset);
    }
    // All GPU sections in one GPU buffer: a 64-byte header of section byte offsets (uint[16]), then
    // the sections (256-byte aligned). Shaders address it through scene_types.slang.
    SDL_GPUBuffer* sceneBuffer() const { return scene_; }
    uint32_t gpuSectionOffset(fpak::Section s) const { return gpuOffsets_[uint32_t(s)]; }

    // The runtime instance table: the cooked instances, then dynamic pools appended before
    // upload(). Pool slots start hidden; their transforms are streamed in by the renderer.
    const std::vector<fpak::InstanceGpu>& instances() const { return instances_; }
    std::vector<fpak::InstanceGpu>& mutableInstances() { return instances_; }  // before upload() only
    uint32_t cookedInstanceCount() const { return cookedInstances_; }
    uint32_t addInstancePool(uint32_t mesh, uint32_t count);
    // glTF node name of a cooked instance (nullptr for pool slots).
    const char* instanceName(uint32_t slot) const
    {
        const fpak::SectionEntry& e = header().sections[uint32_t(fpak::Section::Names)];
        if (slot >= cookedInstances_ || e.size < uint64_t(cookedInstances_) * sizeof(uint32_t))
            return nullptr;
        const char* base = view<char>(fpak::Section::Names);
        uint32_t at;
        std::memcpy(&at, base + slot * sizeof(uint32_t), sizeof(at));
        return at < e.size ? base + at : nullptr;
    }

    // A cooked mesh's name (unique): the glTF node of its first instance, a library mesh's node, or a
    // built-in's name. Paks cooked before mesh names: its first instance's name, or "Mesh N".
    uint32_t cookedMeshCount() const { return header().sections[uint32_t(fpak::Section::Meshes)].count; }
    std::string meshName(uint32_t mesh) const;
    int32_t findMesh(const std::string& name) const;  // -1: none

    // Libraries cooked into the scene (--library: a project's models): where each came from
    // (relative to the project) and its nodes, a mesh and a world transform each (the model's
    // layout, for placing it whole). Empty for paks cooked without library metadata.
    struct LibraryNode {
        uint32_t mesh = 0;
        std::string name;
        float world[3][4] = {};
    };
    struct LibraryCamera {  // a camera of the model (looks down -Z), and its vertical field of view
        std::string name;
        float yfov = 0.8f;  // radians
        float world[3][4] = {};
    };
    struct Library {
        std::string source;
        std::vector<LibraryNode> nodes;
        std::vector<LibraryCamera> cameras;
    };
    const std::vector<Library>& libraries() const { return libraries_; }
    const Library* library(const std::string& source) const;
    int32_t libraryIndex(const std::string& source) const;  // -1: none

    // Rigid node animation (fpak NodeAnimation, baked from glTF node channels): per moving library
    // node or camera (or cooked instance of the scene's own, library fpak::kNodeAnimScene), its
    // world transform (in the library's space) at frames [first, first + count) of a loop of
    // `loop` frames, 12 floats (3x4 rows) per frame.
    struct NodeTrack {
        uint32_t library = 0, target = 0;
        bool camera = false;
        uint32_t first = 0, count = 0, loop = 0;
        const float* rows = nullptr;
    };
    const NodeTrack* nodeTrack(uint32_t library, uint32_t target, bool camera) const;
    const std::vector<NodeTrack>& nodeTracks() const { return nodeTracks_; }
    float nodeFps() const { return nodeFps_; }
    // The library a mesh came from ("" for the scene's own meshes, built-ins and rocks).
    std::string meshSource(uint32_t mesh) const;
    // A material asset (.fmat, relative to the project) cooked into the scene: its material index, or -1.
    int32_t findMaterial(const std::string& source) const;
    const std::vector<std::pair<std::string, uint32_t>>& materialSources() const { return materialSources_; }
    // Materials added before upload() (the editor's spare ones, for material assets made while it
    // runs; runtime copies for scripts).
    uint32_t addMaterials(uint32_t count);
    // Replaces a material here (its GPU copy: MaterialLibrary::flush, once a frame).
    bool setMaterial(uint32_t index, const fpak::MaterialGpu& m);

    // Box3D collision blobs of a mesh, used in place (nullptr when the mesh has none).
    const b3MeshData* staticCollider(uint32_t mesh) const;
    const b3HullData* hullCollider(uint32_t mesh) const;

    // Every mesh and material: the cooked ones, then those added at runtime (the terrain's).
    const std::vector<fpak::MeshGpu>& meshes() const { return meshes_; }
    const std::vector<fpak::MaterialGpu>& materials() const { return materials_; }

    // Terrain patches (render/terrain_renderer.h), reserved before upload(): `patches` slots of one
    // instance, one mesh and one cluster each, `kPatchVertices` vertices and `kPatchTriangles`
    // triangles per cluster (rewritten per patch), and the terrain material. Every pass draws them
    // like any cooked cluster. Slots start hidden.
    struct TerrainSlots {
        uint32_t patches = 0;
        uint32_t firstInstance = 0, firstMesh = 0, firstCluster = 0, firstVertex = 0;
        uint32_t firstClusterVertex = 0, firstTriangle = 0;  // the clusters' fixed offsets
        uint32_t material = 0;
    };
    static constexpr uint32_t kPatchVertices = 81;  // 9 x 9 samples (terrain/terrain.h)
    static constexpr uint32_t kPatchTriangles = 128;
    const TerrainSlots& reserveTerrain(uint32_t patches);
    const TerrainSlots& terrainSlots() const { return terrain_; }

    // Live meshes (render/live_geometry.h: level meshes edited in the editor, meshes games build),
    // reserved before upload(), after the terrain's: `meshes` slots of one instance and one mesh
    // each, and `blocks` cluster blocks shared by them. A block is one cluster with room for
    // kLiveBlockVertices vertices and kLiveBlockTriangles triangles; a mesh takes a run of blocks.
    // Slots start hidden.
    struct LiveSlots {
        uint32_t meshes = 0, blocks = 0;
        uint32_t firstInstance = 0, firstMesh = 0, firstCluster = 0, firstVertex = 0;
        uint32_t firstClusterVertex = 0, firstTriangle = 0;
        bool ownsMesh(uint32_t mesh) const { return mesh - firstMesh < meshes; }
    };
    static constexpr uint32_t kLiveBlockVertices = 128;
    static constexpr uint32_t kLiveBlockTriangles = 128;
    const LiveSlots& reserveLive(uint32_t meshes, uint32_t blocks);
    const LiveSlots& liveSlots() const { return live_; }
    // A live mesh's triangle collider (made with b3CreateMesh; owned from here on, destroyed with
    // b3DestroyMesh; nullptr: none). staticCollider() returns it. Each change bumps the mesh's
    // revision, which physics and audio compare to rebuild what they made from the old one.
    void setLiveCollider(uint32_t mesh, b3MeshData* collider);
    uint32_t liveRevision(uint32_t mesh) const { return live_.ownsMesh(mesh) ? liveRevisions_[mesh - live_.firstMesh] : 0; }
    // The CPU copy of a live mesh's row (bounds, for placement and fallback colliders; the GPU's
    // is LiveGeometry's to write).
    void setLiveMeshRow(uint32_t mesh, const fpak::MeshGpu& row)
    {
        if (live_.ownsMesh(mesh) && mesh < meshes_.size())
            meshes_[mesh] = row;
    }
    // Bumped whenever any live mesh changes (its triangles or its collider).
    uint32_t liveGeneration() const { return liveGeneration_; }
    void bumpLive(uint32_t mesh)
    {
        if (live_.ownsMesh(mesh)) {
            ++liveRevisions_[mesh - live_.firstMesh];
            ++liveGeneration_;
        }
    }

private:
    bool validate() const;
    uint64_t gpuSectionSize(uint32_t section) const;

    const char* path_ = nullptr;
    SDL_AsyncIOQueue* queue_ = nullptr;
    SDL_AsyncIOOutcome outcome_ = {};  // the read's, once readDone() saw it
    bool readOutcome_ = false;
    void* data_ = nullptr;
    size_t size_ = 0;
    SDL_GPUBuffer* scene_ = nullptr;
    std::vector<fpak::InstanceGpu> instances_;
    std::vector<fpak::MeshGpu> meshes_;
    std::vector<fpak::MaterialGpu> materials_;
    uint32_t cookedInstances_ = 0;
    std::vector<Library> libraries_;
    std::vector<std::pair<std::string, uint32_t>> materialSources_;
    void readLibraries();  // the Names trailer's "FXMD" metadata
    void readNodeAnimation();
    std::vector<NodeTrack> nodeTracks_;
    float nodeFps_ = 30.0f;
    TerrainSlots terrain_;
    LiveSlots live_;
    std::vector<b3MeshData*> liveColliders_;
    std::vector<uint32_t> liveRevisions_;
    uint32_t liveGeneration_ = 0;
    uint32_t gpuOffsets_[16] = {};
};

}  // namespace fx
