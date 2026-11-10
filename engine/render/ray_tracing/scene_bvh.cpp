#include "render/ray_tracing/scene_bvh.h"

#include "assets/scene_pak.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/profile.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"

#include <TaskScheduler.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

using Clock = std::chrono::steady_clock;

float msSince(Clock::time_point t0) { return std::chrono::duration<float, std::milli>(Clock::now() - t0).count(); }

// Room a BLAS gets in the arenas: a little over its size, so a live mesh or terrain patch that
// grows a bit stays where it is.
uint32_t roomFor(size_t n) { return n ? uint32_t(n + n / 4 + 8) : 0u; }

// A cooked mesh's finest triangles (depth 0 of its cluster DAG), alpha-blended clusters left out.
std::vector<rt::BvhTriangleGpu> cookedTriangles(const ScenePak& pak, uint32_t mesh)
{
    std::vector<rt::BvhTriangleGpu> out;
    using S = fpak::Section;
    uint32_t clusterCount = 0, vertexCount = 0, clusterVertexCount = 0, triangleCount = 0;
    const auto* clusters = pak.view<fpak::ClusterGpu>(S::Clusters, &clusterCount);
    const auto* vertices = pak.view<fpak::VertexGpu>(S::Vertices, &vertexCount);
    const auto* clusterVertices = pak.view<uint32_t>(S::ClusterVertices, &clusterVertexCount);
    const auto* triangles = pak.view<uint32_t>(S::ClusterTriangles, &triangleCount);
    const fpak::MeshGpu& m = pak.meshes()[mesh];
    if (uint64_t(m.clusterOffset) + m.clusterCount > clusterCount)
        return out;
    const auto& materials = pak.materials();
    for (uint32_t c = m.clusterOffset; c < m.clusterOffset + m.clusterCount; ++c) {
        const fpak::ClusterGpu& cl = clusters[c];
        if (cl.depth != 0)
            continue;
        const uint32_t nv = cl.counts & 0xff, nt = cl.counts >> 8 & 0xff, material = cl.counts >> 16;
        if (material < materials.size() && (materials[material].flags & fpak::kMaterialAlphaBlend))
            continue;
        if (uint64_t(cl.vertexOffset) + nv > clusterVertexCount || uint64_t(cl.triangleOffset) + nt > triangleCount)
            continue;
        for (uint32_t t = 0; t < nt; ++t) {
            const uint32_t packed = triangles[cl.triangleOffset + t];
            rt::BvhTriangleGpu tri;
            float* p[3] = {tri.v0, tri.v1, tri.v2};
            bool ok = true;
            for (int k = 0; k < 3 && ok; ++k) {
                const uint32_t g = clusterVertices[cl.vertexOffset + (packed >> (8 * k) & 0xff)];
                if (g >= vertexCount) {
                    ok = false;
                    break;
                }
                const fpak::VertexGpu& v = vertices[g];
                p[k][0] = m.posOffset[0] + float(v.posXY & 0xffff) * m.posScale[0];
                p[k][1] = m.posOffset[1] + float(v.posXY >> 16) * m.posScale[1];
                p[k][2] = m.posOffset[2] + float(v.posZ & 0xffff) * m.posScale[2];
            }
            if (!ok)
                continue;
            tri.id = rt::triangleId(c, t);
            out.push_back(tri);
        }
    }
    return out;
}

void transform(const float world[3][4], const float p[3], float out[3])
{
    for (int r = 0; r < 3; ++r)
        out[r] = world[r][0] * p[0] + world[r][1] * p[1] + world[r][2] * p[2] + world[r][3];
}

}  // namespace

bool SceneBvh::init(SDL_GPUDevice* device, const ScenePak& pak)
{
    FX_ZONE_CAT("SceneBvh::init", Rendering);
    device_ = device;
    pak_ = &pak;
    blas_.assign(pak.meshes().size(), Blas{});
    const auto t0 = Clock::now();
    const uint32_t cooked = std::min<uint32_t>(pak.cookedMeshCount(), uint32_t(blas_.size()));
    enki::TaskSet task(cooked, [&](enki::TaskSetPartition range, uint32_t) {
        FX_ZONE_CAT("BLAS build", Rendering);
        for (uint32_t m = range.start; m < range.end; ++m)
            buildBlas(blas_[m], cookedTriangles(pak, m));
    });
    jobs::scheduler().AddTaskSetToPipe(&task);
    jobs::scheduler().WaitforTask(&task);
    stats_.loadBuildMs = msSince(t0);
    for (const Blas& b : blas_)
        if (!b.triangles.empty()) {
            ++stats_.meshes;
            stats_.triangles += uint32_t(b.triangles.size());
        }

    // Room for what is written while running: every terrain patch and live block full.
    uint64_t nodes = 0, tris = 0;
    for (const Blas& b : blas_) {
        nodes += roomFor(b.nodes.size());
        tris += roomFor(b.triangles.size());
    }
    const ScenePak::TerrainSlots& ts = pak.terrainSlots();
    const ScenePak::LiveSlots& ls = pak.liveSlots();
    tris += uint64_t(ts.patches) * roomFor(ScenePak::kPatchTriangles) + uint64_t(ls.blocks) * ScenePak::kLiveBlockTriangles;
    nodes += uint64_t(ts.patches) * roomFor(ScenePak::kPatchTriangles / 2) + uint64_t(ls.blocks) * ScenePak::kLiveBlockTriangles / 2;
    const uint32_t instances = uint32_t(pak.instances().size()) + ts.patches;
    repack_ = true;
    if (!ensureBuffer(std::max(instances, 1u), std::max(instances, 1u), uint32_t(std::min<uint64_t>(nodes + 64, 1u << 26)),
                      uint32_t(std::min<uint64_t>(tris + 64, 1u << 26))))
        return false;
    FX_INFO("ray tracing: %u meshes, %u triangles, %.1f MB, BLAS built in %.0f ms", stats_.meshes, stats_.triangles,
            double(bufferBytes_) / (1024.0 * 1024.0), double(stats_.loadBuildMs));
    return true;
}

void SceneBvh::shutdown()
{
    if (!device_)
        return;
    gpumem::releaseBuffer(device_, buffer_);
    buffer_ = nullptr;
    gpumem::releaseTransferBuffer(device_, upload_);
    upload_ = nullptr;
    uploadBytes_ = 0;
    blas_.clear();
    records_.clear();
    members_.clear();
    boxes_.clear();
    local_.clear();
    inTlas_.clear();
    device_ = nullptr;
    stats_ = {};
}

SceneBvh::BuiltMesh SceneBvh::buildMesh(std::vector<rt::BvhTriangleGpu> triangles)
{
    BuiltMesh out;
    out.triangles = std::move(triangles);
    if (out.triangles.empty())
        return out;
    rt::Bvh bvh;
    rt::buildTriangles(out.triangles, bvh);
    out.bounds = rt::rootBounds(bvh);
    out.nodes = std::move(bvh.nodes);
    return out;
}

void SceneBvh::buildBlas(Blas& b, std::vector<rt::BvhTriangleGpu>&& triangles)
{
    BuiltMesh built = buildMesh(std::move(triangles));
    b.triangles = std::move(built.triangles);
    b.nodes = std::move(built.nodes);
    b.bounds = built.bounds;
    b.uploaded = false;
}

bool SceneBvh::place(Blas& b)
{
    const uint32_t n = uint32_t(b.nodes.size()), t = uint32_t(b.triangles.size());
    if (n <= b.nodeRoom && t <= b.triRoom)
        return true;
    const uint32_t nr = roomFor(n), tr = roomFor(t);
    if (uint64_t(nodeTop_) + nr > blasNodeCap_ || uint64_t(triTop_) + tr > blasTriCap_)
        return false;
    b.nodeBase = nodeTop_;
    b.nodeRoom = nr;
    b.triBase = triTop_;
    b.triRoom = tr;
    nodeTop_ += nr;
    triTop_ += tr;
    return true;
}

void SceneBvh::setBuilt(uint32_t mesh, BuiltMesh built)
{
    if (mesh >= blas_.size())
        return;
    Blas& b = blas_[mesh];
    const bool had = !b.triangles.empty();
    stats_.triangles -= uint32_t(b.triangles.size());
    b.triangles = std::move(built.triangles);
    b.nodes = std::move(built.nodes);
    b.bounds = built.bounds;
    b.uploaded = false;
    stats_.triangles += uint32_t(b.triangles.size());
    stats_.meshes += uint32_t(!b.triangles.empty()) - uint32_t(had);
    if (!b.triangles.empty() && !place(b))
        repack_ = true;
    rebuild_ = true;  // (its instances' bounds and records)
}

void SceneBvh::rowChanged(uint32_t slot, const fpak::InstanceGpu& before, const fpak::InstanceGpu& after)
{
    const bool was = include(slot, before), is = include(slot, after);
    if (was != is || (is && before.mesh != after.mesh)) {
        rebuild_ = true;
        return;
    }
    if (is && std::memcmp(before.world, after.world, sizeof(before.world)) != 0)
        moved_.push_back(slot);
}

void SceneBvh::setExtraInstances(const std::vector<ExtraInstance>& extra)
{
    extra_ = extra;
    rebuild_ = true;
}

bool SceneBvh::include(uint32_t slot, const fpak::InstanceGpu& row) const
{
    const bool character = (row.flags & fpak::kInstanceSkinned) && !(deformed_ && deformed_->count(slot));
    return !(row.flags & fpak::kInstanceHidden) && !character && row.mesh < blas_.size() &&
           !blas_[row.mesh].triangles.empty();
}

bool SceneBvh::inverse(const float world[3][4], Record& r)
{
    const float(*m)[4] = world;
    const float c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    const float c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    const float c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    const float det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    if (!(std::fabs(det) > 1e-18f))
        return false;
    const float id = 1.0f / det;
    float a[3][3];  // the inverse of the 3x3 part
    a[0][0] = c00 * id;
    a[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id;
    a[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
    a[1][0] = c01 * id;
    a[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
    a[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
    a[2][0] = c02 * id;
    a[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id;
    a[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            r.inverse[i][j] = a[i][j];
        r.inverse[i][3] = -(a[i][0] * m[0][3] + a[i][1] * m[1][3] + a[i][2] * m[2][3]);
    }
    r.instance = (r.instance & ~kInstanceMirrored) | (det < 0.0f ? kInstanceMirrored : 0u);
    return true;
}

namespace {

rt::Aabb worldBox(const float world[3][4], const rt::Aabb& local)
{
    rt::Aabb box;
    for (int corner = 0; corner < 8; ++corner) {
        const float p[3] = {(corner & 1) ? local.hi[0] : local.lo[0], (corner & 2) ? local.hi[1] : local.lo[1],
                            (corner & 4) ? local.hi[2] : local.lo[2]};
        float w[3];
        transform(world, p, w);
        box.grow(w);
    }
    return box;
}

rt::Aabb childBox(const rt::BvhNodeGpu& n, bool right)
{
    rt::Aabb b;
    const float* lo = right ? n.rightLo : n.leftLo;
    const float* hi = right ? n.rightHi : n.leftHi;
    if (lo[0] >= 1e29f)
        return b;  // (empty)
    b.grow(lo);
    b.grow(hi);
    return b;
}

const float kIdentity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};

}  // namespace

void SceneBvh::buildTlas()
{
    const auto t0 = Clock::now();
    const auto& rows = *rows_;
    // The instances: one entry each, its BLAS whole.
    struct Entry {
        uint32_t member, start;
        rt::Aabb local, world;
        float area;
    };
    std::vector<Entry> entries;
    std::vector<Record> base;  // per member: its record without the start
    std::vector<uint32_t> memberIds;
    auto add = [&](uint32_t member, uint32_t instance, const float world[3][4], uint32_t mesh) {
        Record r = {};
        r.instance = instance;
        if (!inverse(world, r))
            return;
        const Blas& b = blas_[mesh];
        r.nodeBase = b.nodeBase;
        r.triBase = b.triBase;
        const rt::Aabb w = worldBox(world, b.bounds);
        entries.push_back({uint32_t(memberIds.size()), 0u, b.bounds, w, w.area()});
        memberIds.push_back(member);
        base.push_back(r);
    };
    inTlas_.assign(rows.size(), 0);
    for (uint32_t s = 0; s < rows.size(); ++s)
        if (include(s, rows[s])) {
            add(s, s, rows[s].world, rows[s].mesh);
            inTlas_[s] = 1;
        }
    for (uint32_t e = 0; e < extra_.size(); ++e) {
        const ExtraInstance& x = extra_[e];
        if (x.mesh < blas_.size() && !blas_[x.mesh].triangles.empty())
            add(kExtraBit | e, x.instance, kIdentity, x.mesh);
    }
    auto meshOf = [&](uint32_t member) {
        return (memberIds[member] & kExtraBit) ? extra_[memberIds[member] & ~kExtraBit].mesh : rows[memberIds[member]].mesh;
    };
    auto worldOf = [&](uint32_t member) -> const float(*)[4] {
        return (memberIds[member] & kExtraBit) ? kIdentity : rows[memberIds[member]].world;
    };

    // Partial re-braiding: open the largest boxes into their BLAS's two children, to about twice
    // the instances, never boxes much smaller than the scene's 1/10000.
    rt::Aabb scene;
    for (const Entry& e : entries)
        scene.grow(e.world);
    const float minArea = scene.area() * 1e-4f;
    const size_t budget = entries.size() * 2;
    auto less = [](const Entry& a, const Entry& b) { return a.area < b.area; };
    std::make_heap(entries.begin(), entries.end(), less);
    std::vector<Entry> done;
    done.reserve(budget);
    while (!entries.empty()) {
        std::pop_heap(entries.begin(), entries.end(), less);
        const Entry e = entries.back();
        entries.pop_back();
        const bool canOpen = !(e.start & rt::kLeafBit) && e.area > minArea && entries.size() + done.size() + 2 <= budget;
        if (!canOpen) {
            done.push_back(e);
            continue;
        }
        const rt::BvhNodeGpu& n = blas_[meshOf(e.member)].nodes[e.start];
        for (int side = 0; side < 2; ++side) {
            const uint32_t c = side ? n.right : n.left;
            if ((c & rt::kLeafBit) && (c & rt::kLeafCountMask) == 0)
                continue;  // (an empty child)
            Entry child;
            child.member = e.member;
            child.start = c;
            child.local = childBox(n, side != 0);
            child.world = worldBox(worldOf(e.member), child.local);
            child.area = child.world.area();
            entries.push_back(child);
            std::push_heap(entries.begin(), entries.end(), less);
        }
    }

    std::vector<rt::Aabb> boxes(done.size());
    for (size_t i = 0; i < done.size(); ++i)
        boxes[i] = done[i].world;
    rt::build(boxes.data(), uint32_t(boxes.size()), 1, tlas_);
    // Records in leaf order, so a leaf's first is its record (and refits read boxes in that order).
    members_.resize(done.size());
    records_.resize(done.size());
    local_.resize(done.size());
    boxes_.resize(done.size());
    for (size_t i = 0; i < tlas_.order.size(); ++i) {
        const Entry& e = done[tlas_.order[i]];
        members_[i] = memberIds[e.member];
        records_[i] = base[e.member];
        records_[i].start = e.start;
        local_[i] = e.local;
        boxes_[i] = e.world;
        tlas_.order[i] = uint32_t(i);
    }
    rebuild_ = false;
    moved_.clear();
    refitsSinceBuild_ = 0;
    tlasUploaded_ = false;
    stats_.instances = uint32_t(memberIds.size());
    stats_.records = uint32_t(records_.size());
    ++stats_.tlasBuilds;
    stats_.lastTlasMs = msSince(t0);
}

void SceneBvh::refitTlas()
{
    const auto t0 = Clock::now();
    const auto& rows = *rows_;
    std::vector<uint8_t> moved(rows.size(), 0);
    bool any = false;
    for (const uint32_t s : moved_)
        if (s < inTlas_.size() && inTlas_[s]) {
            moved[s] = 1;
            any = true;
        }
    moved_.clear();
    if (!any)
        return;
    for (size_t i = 0; i < records_.size(); ++i) {
        const uint32_t s = members_[i];
        if ((s & kExtraBit) || !moved[s])
            continue;
        if (!inverse(rows[s].world, records_[i])) {
            buildTlas();  // (scaled to nothing: dropped by a rebuild)
            return;
        }
        boxes_[i] = worldBox(rows[s].world, local_[i]);
    }
    rt::refit(boxes_.data(), tlas_);
    ++refitsSinceBuild_;
    // Moved far enough that the tree no longer fits them: build it again.
    if (rt::sahCost(tlas_) > 1.5f * tlas_.cost) {
        buildTlas();
        return;
    }
    tlasUploaded_ = false;
    ++stats_.tlasRefits;
    stats_.lastTlasMs = msSince(t0);
}

bool SceneBvh::ensureBuffer(uint32_t tlasNodes, uint32_t instances, uint32_t blasNodes, uint32_t blasTriangles)
{
    if (buffer_ && !repack_ && tlasNodes <= tlasNodeCap_ && instances <= instanceCap_ && blasNodes <= blasNodeCap_ &&
        blasTriangles <= blasTriCap_)
        return true;
    // Larger: the TLAS with room to grow, the arenas at what they need (packed) plus a quarter.
    tlasNodeCap_ = std::max(tlasNodeCap_, tlasNodes + tlasNodes / 2 + 16);
    instanceCap_ = std::max(instanceCap_, instances + instances / 2 + 16);
    uint64_t packedNodes = 0, packedTris = 0;
    for (const Blas& b : blas_) {
        packedNodes += roomFor(b.nodes.size());
        packedTris += roomFor(b.triangles.size());
    }
    blasNodeCap_ = uint32_t(std::max<uint64_t>({blasNodes, packedNodes + packedNodes / 4 + 64, blasNodeCap_}));
    blasTriCap_ = uint32_t(std::max<uint64_t>({blasTriangles, packedTris + packedTris / 4 + 64, blasTriCap_}));
    tlasNodesAt_ = kHeaderBytes;
    instancesAt_ = tlasNodesAt_ + tlasNodeCap_ * uint32_t(sizeof(rt::BvhNodeGpu));
    blasNodesAt_ = instancesAt_ + instanceCap_ * kInstanceBytes;
    const uint64_t trisAt = uint64_t(blasNodesAt_) + uint64_t(blasNodeCap_) * sizeof(rt::BvhNodeGpu);
    const uint64_t bytes = trisAt + uint64_t(blasTriCap_) * sizeof(rt::BvhTriangleGpu);
    if (bytes > 0x7fffffffull) {
        FX_ERROR("ray tracing: the scene needs %.0f MB, more than a buffer holds", double(bytes) / (1024.0 * 1024.0));
        return false;
    }
    blasTrisAt_ = uint32_t(trisAt);
    gpumem::releaseBuffer(device_, buffer_);
    SDL_GPUBufferCreateInfo info = {};
    info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    info.size = uint32_t(bytes);
    buffer_ = gpumem::createBuffer("Ray tracing", device_, &info);
    if (!buffer_) {
        FX_ERROR("ray tracing: %s", SDL_GetError());
        return false;
    }
    gpumem::setBufferName(device_, buffer_, "ray tracing bvh");
    bufferBytes_ = bytes;
    stats_.bytes = bytes;
    // Everything packed again from the start of the arenas.
    nodeTop_ = triTop_ = 0;
    for (Blas& b : blas_) {
        b.nodeRoom = b.triRoom = 0;
        b.uploaded = false;
        if (!b.triangles.empty())
            place(b);
    }
    writes_.clear();
    staging_.clear();
    repack_ = false;
    rebuild_ = true;  // (records hold the bases)
    return true;
}

void SceneBvh::stage(uint32_t offset, const void* data, uint32_t bytes)
{
    if (!bytes)
        return;
    writes_.push_back({offset, bytes, uint32_t(staging_.size())});
    const uint8_t* b = static_cast<const uint8_t*>(data);
    staging_.insert(staging_.end(), b, b + bytes);
}

void SceneBvh::update(SDL_GPUCommandBuffer* cmd)
{
    if (!device_ || !rows_)
        return;
    FX_ZONE_CAT("SceneBvh::update", Rendering);
    if (repack_ && !ensureBuffer(tlasNodeCap_, instanceCap_, blasNodeCap_, blasTriCap_))
        return;
    if (rebuild_)
        buildTlas();
    else if (!moved_.empty())
        refitTlas();
    if (!ensureBuffer(uint32_t(tlas_.nodes.size()), uint32_t(records_.size()), nodeTop_, triTop_))
        return;
    if (rebuild_)  // (the buffer was made again: records carry the new bases)
        buildTlas();

    for (Blas& b : blas_) {
        if (b.uploaded || b.triangles.empty())
            continue;
        stage(blasNodesAt_ + b.nodeBase * uint32_t(sizeof(rt::BvhNodeGpu)), b.nodes.data(),
              uint32_t(b.nodes.size() * sizeof(rt::BvhNodeGpu)));
        stage(blasTrisAt_ + b.triBase * uint32_t(sizeof(rt::BvhTriangleGpu)), b.triangles.data(),
              uint32_t(b.triangles.size() * sizeof(rt::BvhTriangleGpu)));
        b.uploaded = true;
    }
    if (!tlasUploaded_) {
        const uint32_t header[8] = {tlasNodesAt_, instancesAt_, blasNodesAt_, blasTrisAt_, uint32_t(records_.size()),
                                    uint32_t(tlas_.nodes.size()), 0, 0};
        stage(0, header, sizeof(header));
        stage(tlasNodesAt_, tlas_.nodes.data(), uint32_t(tlas_.nodes.size() * sizeof(rt::BvhNodeGpu)));
        stage(instancesAt_, records_.data(), uint32_t(records_.size() * kInstanceBytes));
        tlasUploaded_ = true;
    }
    if (writes_.empty()) {
        // A big transfer buffer (the load's) is not kept once nothing needs it.
        if (upload_ && uploadBytes_ > (16u << 20)) {
            gpumem::releaseTransferBuffer(device_, upload_);
            upload_ = nullptr;
            uploadBytes_ = 0;
        }
        return;
    }
    const uint32_t bytes = uint32_t(staging_.size());
    if (bytes > uploadBytes_ || (uploadBytes_ > (16u << 20) && bytes < (1u << 20))) {
        gpumem::releaseTransferBuffer(device_, upload_);
        SDL_GPUTransferBufferCreateInfo t = {};
        t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        t.size = std::max(bytes, 1u << 20);
        upload_ = gpumem::createTransferBuffer("Ray tracing", device_, &t, "ray tracing upload");
        uploadBytes_ = upload_ ? t.size : 0;
        if (!upload_) {
            FX_ERROR("ray tracing: %s", SDL_GetError());
            writes_.clear();
            staging_.clear();
            return;
        }
    }
    void* mapped = SDL_MapGPUTransferBuffer(device_, upload_, true);
    if (mapped) {
        std::memcpy(mapped, staging_.data(), bytes);
        SDL_UnmapGPUTransferBuffer(device_, upload_);
        FX_GPU_PASS(cmd, "bvh upload");
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        for (const Write& w : writes_) {
            const SDL_GPUTransferBufferLocation src = {upload_, w.src};
            const SDL_GPUBufferRegion dst = {buffer_, w.offset, w.bytes};
            SDL_UploadToGPUBuffer(copy, &src, &dst, false);
        }
        SDL_EndGPUCopyPass(copy);
    }
    writes_.clear();
    staging_.clear();
}

}  // namespace fx
