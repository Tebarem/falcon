#pragma once
// Navigation for the level tools: a navigation mesh baked from a scene's walkable surfaces (Recast,
// in tiles), and the queries on it (Detour): a path between two points, a straight-line walk that
// stops at the mesh's edge, the nearest point on it. Saved as a scene's sidecar ("<scene>.nav").

#include "core/math.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class dtCrowd;
class dtNavMesh;
class dtNavMeshQuery;

namespace fx::level {

struct NavSettings {
    float cellSize = 0.2f, cellHeight = 0.1f;  // the voxels it is baked from (metres)
    float agentHeight = 1.8f, agentRadius = 0.35f, agentClimb = 0.4f;
    float maxSlope = 45.0f;  // degrees
    uint32_t tileSize = 48;  // cells a tile
    bool operator==(const NavSettings&) const = default;
};

// A volume over it (level_meshes.h LevelVolume): a footprint (a convex outline in xz, its points
// counter-clockwise from above) between two heights; a blocker (nothing walks there) or an area that
// costs `cost` times as much to walk through (paths go round it when they can).
struct NavVolume {
    std::vector<float3> outline;
    float bottom = 0.0f, top = 0.0f;
    bool blocks = true;
    float cost = 2.0f;
};

// What it is baked from: world triangles (three indices each), each walkable or not (an obstacle:
// it blocks, nothing walks on it), and volumes.
struct NavInput {
    std::vector<float3> vertices;
    std::vector<uint32_t> indices;
    std::vector<uint8_t> walkable;  // per triangle (empty: all)
    std::vector<NavVolume> volumes;
};

class NavMesh {
public:
    NavMesh();
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    bool build(const NavInput& input, const NavSettings& settings, std::string* error = nullptr);
    bool valid() const { return nav_ != nullptr; }
    void clear();
    std::vector<uint8_t> save() const;
    bool load(const uint8_t* data, size_t size);

    // A path from `from` to `to` (each found on the mesh within a couple of metres): its corners,
    // the first and last the points on the mesh. False: either is off the mesh, or no way between
    // (then the path goes as near as it can).
    bool findPath(float3 from, float3 to, std::vector<float3>& path) const;
    // A straight walk from `from` towards `to` along the mesh: false when it reaches it; true when
    // an edge stops it at `hit` (`t`: how far along, 0..1).
    bool raycast(float3 from, float3 to, float& t, float3& hit) const;
    bool nearest(float3 p, float3& out, float3 extents = {2.0f, 4.0f, 2.0f}) const;

    uint32_t tileCount() const;
    uint32_t polygonCount() const;
    const NavSettings& settings() const { return settings_; }
    // Its polygons' edges as line pairs (world), a little above them: the debug view.
    void edges(std::vector<float3>& lines) const;
    dtNavMesh* detour() const { return nav_; }  // (the crowd's)
    const float* areaCosts() const { return areaCost_; }

private:
    dtNavMesh* nav_ = nullptr;
    dtNavMeshQuery* query_ = nullptr;
    NavSettings settings_;
    float areaCost_[64] = {};  // (each area's: the cost areas' from their volumes, the rest 1)
};

// Agents walking a navigation mesh together (Detour's crowd): each steered along its path to its
// target, keeping clear of the others. Ids carry the crowd's generation (a new crowd: old ids
// stale).
class NavCrowd {
public:
    NavCrowd(std::shared_ptr<const NavMesh> nav, uint32_t generation);
    ~NavCrowd();
    NavCrowd(const NavCrowd&) = delete;
    NavCrowd& operator=(const NavCrowd&) = delete;

    int32_t add(float3 position, float radius, float height, float speed, float acceleration);  // -1: none
    void remove(int32_t id);
    bool target(int32_t id, float3 to);  // false: not on the mesh near there
    void stop(int32_t id);
    void update(float dt);
    // Where it is and its velocity; how far it still has to go (0: arrived or no target; -1: no
    // such agent).
    float state(int32_t id, float3& position, float3& velocity) const;
    const NavMesh* mesh() const { return nav_.get(); }

private:
    int32_t index(int32_t id) const;
    std::shared_ptr<const NavMesh> nav_;
    dtCrowd* crowd_ = nullptr;
    uint32_t generation_;
};

// The scene's navigation mesh (null: none): its sidecar read when a scene starts, replaced by an
// editor's bake. Scripts' queries go to it (script/level_api.h).
std::shared_ptr<const NavMesh> navigation();
void setNavigation(std::shared_ptr<const NavMesh> nav);
std::string navMeshPathFor(const std::string& scenePath);  // <scene without .fpak>.nav
bool loadNavigation(const std::string& scenePath);          // (none there: no navigation)
// The crowd on the scene's navigation mesh (made on first use; a new one with a new mesh; null: none).
NavCrowd* navCrowd();

}  // namespace fx::level
