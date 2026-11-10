#pragma once

// C ABI of the level tools (live meshes now; the polygon-mesh kernel, cuts and navigation as they
// land), its own table with its own version so it changes without the main one
// (script_api.h FxNativeApi::level). Mirrored in managed/Falcon.Engine/Native/LevelApi.cs.

#include <cstdint>

extern "C" {

inline constexpr uint32_t kFxLevelApiVersion = 9;

// (v3) Modifiers written in C# (level/modifiers.h ScriptModifierHandler): op 0 runs class `cls` with
// its `settings` (JSON) on a mesh blob, the result's blob into `out`; op 1 lists the classes as JSON.
// Bytes written, -(bytes needed) when `capacity` is short, 0 when it failed.
typedef int32_t (*FxModifierHandler)(int32_t op, const char* cls, const char* settings, const uint8_t* blob, int32_t size, uint8_t* out,
                                     int32_t capacity);

// A live mesh's triangles (render/live_geometry.h LiveMeshData), in its entity's space. uvs,
// tangents and materials may be null (no UVs; MikkTSpace tangents from the UVs; material 0).
struct FxLiveMeshData {
    const float* positions;   // xyz per vertex
    const float* normals;     // xyz per vertex
    const float* uvs;         // uv per vertex
    const float* tangents;    // xyzw per vertex
    const uint32_t* indices;  // 3 per triangle
    const uint32_t* materials;  // scene material index per triangle
    int32_t vertexCount;
    int32_t indexCount;
    uint32_t flags;  // 1: no collider
    int32_t pad;
};

struct FxLiveStats {
    uint32_t meshes, meshCapacity;
    uint32_t blocks, blockCapacity;  // cluster blocks (128 vertices and triangles each)
    uint32_t triangles;
    uint32_t uploads;  // meshes written since the scene started
    float lastBuildMs;  // the last write's clustering, quantizing and collider
    uint32_t pad;
};

// The editor's scene view as editor code sees it (FxLevelApi::viewport): the mouse, the camera's ray
// through it, and what happened since the last call. Zero outside the editor.
struct FxViewportState {
    float mouse[2];     // view pixels from the top left
    float size[2];      // the view's size in pixels
    float rayOrigin[3], rayDir[3];  // the camera's ray through the mouse (world)
    float cameraPos[3], cameraForward[3], cameraRight[3], cameraUp[3];
    float p00, p11;     // projection scales (x and y)
    uint32_t buttons;   // held: 1 left, 2 right, 4 middle
    uint32_t pressed, released;  // since the last call (same bits)
    uint32_t mods;      // held: 1 shift, 2 ctrl, 4 alt
    float wheel;        // since the last call
    uint32_t inside;    // the mouse is over the view
    uint32_t captured;  // editor code has the view's clicks and keys (viewportCapture)
    uint32_t elementMode;  // 0 objects, 1 vertices, 2 edges, 3 faces
    int32_t keyCount;   // keys pressed since the last call (SDL scancodes, in order; 16 at most)
    uint32_t keys[16];
    char text[32];      // text typed since the last call (UTF-8, NUL-terminated): numbers for modal operations
};

struct FxLevelApi {
    uint32_t version;
    uint32_t size;  // sizeof(FxLevelApi)
    void* context;
    // A live mesh and its entity (at `rows`, a 3x4 world transform; under `parent`, 0: none): the
    // live mesh's index (-1: none left, or no renderer), its entity in *entity. Empty until set.
    // The game's end (the editor's Stop) destroys the game's; so does destroying the entity.
    int32_t (*liveCreate)(void* ctx, const float* rows, uint64_t parent, uint64_t* entity);
    // Its triangles replaced: 1, or 0 (bad data, no room: it keeps what it had).
    int32_t (*liveSet)(void* ctx, int32_t live, const FxLiveMeshData* data);
    void (*liveDestroy)(void* ctx, int32_t live);
    // The live mesh an entity draws (-1: none).
    int32_t (*liveOf)(void* ctx, uint64_t entity);
    // The triangle (its index in the indices set, / 3) that a pick's cluster and triangle are (-1:
    // not this mesh's).
    int32_t (*liveTriangle)(void* ctx, int32_t live, uint32_t cluster, uint32_t triangle);
    void (*liveStats)(void* ctx, FxLiveStats* out);

    // Level meshes (level/level_meshes.h): entities with an editable mesh, saved with the scene.
    // Meshes go both ways as blobs (level/poly_mesh.h PolyMesh::save, mirrored by Falcon.Level.PolyMesh).
    // - meshGet: an entity's mesh copied into `buffer`: its length, -length when `capacity` is
    //   short, 0 when it has none.
    // - meshSet: an entity's mesh replaced (1; 0: no such entity, a bad blob). In the editor while
    //   editing, an undo step (grouped by FxEditorApi::undoBegin / undoEnd).
    // - meshCreate: a new level mesh entity at `rows` under `parent` (0: none) with the blob's mesh
    //   (null: a 1 m box); its entity, 0 when it can't. In the editor while editing it is created
    //   like Create > Level Mesh (an undo step, selected); while playing the game's end deletes it.
    int32_t (*meshGet)(void* ctx, uint64_t entity, uint8_t* buffer, int32_t capacity);
    int32_t (*meshSet)(void* ctx, uint64_t entity, const uint8_t* blob, int32_t size);
    uint64_t (*meshCreate)(void* ctx, const float* rows, uint64_t parent, const uint8_t* blob, int32_t size);

    // The editor's scene view (editor only; 0 / nothing in a game):
    // - viewport: its state now and what happened since the last call (1; 0: no editor).
    // - viewportCapture: while on, the view leaves clicks, drags and keys to editor code (a modal
    //   operation); the camera still flies. Off again at Play and when the code is reloaded. `on`
    //   with bit 2 (2): the mouse wheel is the operation's too (a bevel's segments), not a zoom.
    // - viewportHint: text for the key hints bar under the view ("" back to the mode's keys).
    int32_t (*viewport)(void* ctx, FxViewportState* out);
    void (*viewportCapture)(void* ctx, int32_t on);
    void (*viewportHint)(void* ctx, const char* utf8);
    // The element selection of the level tools: its entity (the primary selection), mode and items
    // (vertex indices, edge keys lo << 32 | hi, face indices). elementsGet copies up to `capacity`
    // and returns how many there are; elementsSet replaces them (and the mode: 1..3; 0 objects).
    int32_t (*elementsGet)(void* ctx, uint64_t* entity, uint32_t* mode, uint64_t* items, int32_t capacity);
    void (*elementsSet)(void* ctx, uint64_t entity, uint32_t mode, const uint64_t* items, int32_t count);
    // A level mesh shown while an operation goes on (no undo step; 1, 0: bad blob or entity), and
    // the operation's end: one undo step from the mesh it started with (`before`). In a game, set.
    int32_t (*meshSetLive)(void* ctx, uint64_t entity, const uint8_t* blob, int32_t size);
    int32_t (*meshCommit)(void* ctx, uint64_t entity, const uint8_t* before, int32_t size);

    // A mesh operation (level/mesh_ops.h: "extrude", "bevel", "shape", ...) on a mesh blob (null for
    // "shape") with its arguments as JSON text. Its result in `buffer`: [u32 n][mesh blob, n bytes]
    // [u32 m][detached mesh blob, m bytes (0: none)][JSON text: {"ok":bool,"mode":n,"selection":[...],
    // "error":"..."}] (not NUL-terminated). Returns the bytes written, or -(bytes needed) when
    // `capacity` is short (run it again: operations are deterministic); 0: bad arguments.
    int32_t (*meshOp)(void* ctx, const char* op, const uint8_t* blob, int32_t size, const char* argsJson, uint8_t* buffer,
                      int32_t capacity);

    // (v3) The scripts' C# modifiers (Falcon.Level.MeshModifier classes): run and listed through
    // `handler` (given again after a reload of the code: the meshes with them made again).
    void (*modifierHandler)(void* ctx, FxModifierHandler handler);

    // (v4) The scene's navigation mesh (level/navmesh.h; baked in the editor, saved beside the scene):
    // - navPath: a path's corners (xyz each) into `out`, at most `capacity`: how many (negative:
    //   the way is not whole, those go as near as it can; 0: no path, or no navigation mesh).
    // - navRaycast: a straight walk along the mesh: 1 stopped at an edge (`hit`), 0 got there, -1 none.
    // - navNearest: the nearest point on it (1; 0 none near, -1 no navigation mesh).
    int32_t (*navPath)(void* ctx, const float* from, const float* to, float* out, int32_t capacity);
    int32_t (*navRaycast)(void* ctx, const float* from, const float* to, float* hit);
    int32_t (*navNearest)(void* ctx, const float* point, const float* extents, float* out);

    // (v5) Gameplay volumes (level/level_meshes.h LevelVolume):
    // - volumeKind: an entity's (0 trigger, 1 kill, 2 ladder, 3 audio, 4 nav blocker, 5 nav area;
    //   -1: not a volume); its tag into `tag` (NUL-terminated, cut to `capacity`).
    // - volumesAt: the volumes a point is in (inside its mesh's bounds, as it is turned): their
    //   entities and kinds, at most `capacity`; how many there are.
    int32_t (*volumeKind)(void* ctx, uint64_t entity, char* tag, int32_t capacity);
    int32_t (*volumesAt)(void* ctx, const float* point, uint64_t* entities, int32_t* kinds, int32_t capacity);

    // (v6) Agents on the navigation mesh (level/navmesh.h NavCrowd; Falcon.Level.NavAgent):
    // - crowdAdd: one at `position` (radius, height, top speed, acceleration): its id (-1: none).
    // - crowdTarget: where it walks to (1; 0: not on the mesh near there; `to` null: it stops).
    // - crowdUpdate: all of them a step on (once a frame).
    // - crowdState: where it is and its velocity; how far it still has to go (0: there, or no
    //   target; -1: no such agent: a new navigation mesh's crowd has none of the old ids).
    int32_t (*crowdAdd)(void* ctx, const float* position, float radius, float height, float speed, float acceleration);
    void (*crowdRemove)(void* ctx, int32_t id);
    int32_t (*crowdTarget)(void* ctx, int32_t id, const float* to);
    void (*crowdUpdate)(void* ctx, float dt);
    float (*crowdState)(void* ctx, int32_t id, float* position, float* velocity);

    // (v7) Cutting while the game runs (Falcon.Level.Cuts):
    // - cutSet: an entity a cut volume (shape 0 box, 1 cylinder, 2 sphere, 3 its own level mesh,
    //   4 damage; -1: not one any more), sized by its transform; `cap`: its faces' material ("" or
    //   null: the target's). 1; 0 no such entity.
    // - geometryHash: the level as it is drawn (LevelMeshes::geometryHash; 0: none).
    int32_t (*cutSet)(void* ctx, uint64_t entity, int32_t shape, int32_t intersect, uint32_t seed, float roughness, const char* cap);
    uint64_t (*geometryHash)(void* ctx);

    // (v8) cutsPending: cuts being made on worker threads now (their meshes drawn as they were).
    int32_t (*cutsPending)(void* ctx);

    // (v9) toolState: the level commands' state for the editor's Level window, JSON ({"options":
    // {path: {name: value}}}), given when it changes; the page gets it (editor.tools.state).
    void (*toolState)(void* ctx, const char* json);
};

}  // extern "C"

#include <memory>

// What the editor gives the level API (EditorBridge implements it; null in a game).
namespace fx {
namespace level {
class PolyMesh;
}
class LevelEditorLink {
public:
    virtual ~LevelEditorLink() = default;
    virtual bool viewportState(FxViewportState& out) = 0;
    virtual void viewportCapture(bool on, bool wheel = false) = 0;
    virtual void viewportHint(const char* text) = 0;
    virtual void toolState(const char* json) = 0;
    virtual int32_t elementsGet(uint64_t& entity, uint32_t& mode, uint64_t* items, int32_t capacity) = 0;
    virtual void elementsSet(uint64_t entity, uint32_t mode, const uint64_t* items, int32_t count) = 0;
    // An editor entity's level mesh shown live, and committed as one undo step (false: not the editor's).
    virtual bool meshLive(uint64_t entity, std::shared_ptr<const level::PolyMesh> mesh) = 0;
    virtual bool meshCommit(uint64_t entity, std::shared_ptr<const level::PolyMesh> before) = 0;
};
}  // namespace fx
