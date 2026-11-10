#pragma once

// The editor's scene view: what the viewport shows and does on top of the rendered frame.
//   - tools (Unity keys): Q view, W move, E rotate, R scale; X global/local, Z pivot/center,
//     F frame the selection, hold Ctrl while dragging to snap
//   - transform gizmo: axis and plane handles, rotation rings (plus a view ring), scale
//     handles; drags apply live and land as one undo step
//   - click to select (GPU pick from the visibility buffer), click empty space to clear
//   - orientation gizmo (top right): click an axis to look along it
//   - B: the terrain brush (TerrainEditor): drag on the terrain to sculpt, Shift inverts, Ctrl
//     smooths, [ and ] resize it
//   - infinite ground grid (depth-tested) and the selection outline
//   - Shift+click adds to the selection, Ctrl+click toggles; drag on empty space for a box
//   - snapping while dragging: Ctrl to the grid (absolute: positions land on grid lines), V to
//     the nearest vertex or edge, Ctrl+Shift onto the surface under the mouse
//   - element modes of a level mesh: 1 objects, 2 vertices, 3 edges, 4 faces (click, box, gizmo)
// Mouse coordinates are framebuffer pixels relative to the viewport.

#include "core/math.h"
#include "editor/entity_marker.h"
#include "script/level_api.h"
#include "ui/ui_layer.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace fx {

class EditorBridge;
class FlyCamera;
class GpuDevice;
class GpuDrivenRenderer;
class ScenePak;
struct CameraState;

// Paint: the selected level mesh's mask or colours; Spline: the selected spline's points (or a new one);
// Measure: a ruler dragged between two points on surfaces (V: onto vertices).
enum class SceneTool : uint32_t { View, Move, Rotate, Scale, Brush, Paint, Spline, Measure };
// What clicks and the gizmo act on: entities, or the elements of the selected level mesh.
enum class ElementMode : uint32_t { Object, Vertex, Edge, Face };

namespace level {
class PolyMesh;
}

struct SceneToolState {
    SceneTool tool = SceneTool::Move;
    bool local = false;        // gizmo axes: entity (local) or world
    // Gizmo at the mesh bounds' center (Unity's Center) instead of the entity origin (Pivot). The
    // default: glTF scenes often keep every node at the world origin with baked vertices.
    bool pivotCenter = true;
    bool grid = true;
    bool gameView = false;     // Game tab: no grid, gizmos or outline
    float snapMove = 0.25f, snapRotate = 15.0f, snapScale = 0.1f;
    bool snapAbsolute = true;  // grid snapping puts positions on grid lines (else snaps the move's length)
    ElementMode element = ElementMode::Object;
    // The level paint brush (SceneTool::Paint, editor/level_paint.cpp): what it paints (0..2 the mask's
    // layers 1..3, 3 erasing them, 4 vertex colours, 5 decals stamped on any surface, 6 damage cut
    // into level meshes), its radius (metres), strength and colour (RGBA8), the decals' material,
    // where damage goes (0 holes, 1 chipped edges, 2 broken corners) and what its cuts' faces get.
    uint32_t paintMode = 0;
    std::string paintDecal;
    uint32_t paintDamage = 0;
    bool paintRubble = true;  // (damage) a heap of rubble under each hole in a wall or ceiling
    std::string paintCap;
    // (damage over the selection, editor.level.damage) the chips' and holes' spacing in metres (0:
    // from the brush, 1.5 radii; holes 2.5), how far they stray and vary in size (0..1) and the
    // share of their places that get one (0..1).
    float damageSpacing = 0.0f, damageRandomness = 0.0f, damageDensity = 1.0f;
    std::string splineProfile;  // (the spline tool) what a new spline is lofted into ("": none)
    // (paint mode 7, the scatter brush) its prefabs, pieces a square metre, their size range, none
    // nearer each other than `scatterSpacing` metres, none on a surface steeper than `scatterSlope`
    // degrees, turned to the surface (`scatterAlign`) or upright.
    std::vector<std::string> scatterPrefabs;
    float scatterDensity = 1.0f, scatterSpacing = 0.5f, scatterSlope = 45.0f;
    float scatterScale[2] = {0.8f, 1.2f};
    bool scatterAlign = false;
    bool showNav = false;  // the navigation mesh's polygons drawn over the view
    bool orthographic = false;  // (the camera's: Numpad 5)
    float paintRadius = 0.5f, paintStrength = 0.5f;
    uint32_t paintColor = 0xffffffffu;
};

class SceneView {
public:
    bool init(const GpuDevice& gpu, GpuDrivenRenderer& renderer, ScenePak& pak, FlyCamera& camera, EditorBridge& bridge);
    void shutdown();

    // Events the UI layer gave to the game. Returns true when the scene view used them (the
    // camera still gets every event: it ignores plain left clicks and tool keys itself).
    bool handleEvent(const SDL_Event& e, float pixelScaleX, float pixelScaleY);
    // After the frame's render(): pick answers, hover.
    void update(const UiRect& viewport, const CameraState& camera);
    // Lines in the world drawn over the view this frame (scripts' Debug.*: world a, b, RGBA8).
    struct WorldLine {
        float3 a, b;
        uint32_t color;
    };
    void setWorldLines(std::vector<WorldLine> lines) { worldLines_ = std::move(lines); }
    // A tool's outline of what a click would make (the opening tool's door in a wall); {}: none.
    void setPreviewLines(std::vector<WorldLine> lines) { previewLines_ = std::move(lines); }
    const std::vector<WorldLine>& previewLines() const { return previewLines_; }
    // Grid, outline and gizmos over the scene, in the viewport of the swapchain image.
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target);

    SceneToolState& tools() { return tools_; }
    uint32_t damageShown() const;  // (tests) the damage cuts drawn last frame
    uint32_t cutsShown() const;    // (tests) the cut volumes drawn (openings' too)
    uint32_t outlinesShown() const;  // (tests) the gameplay volumes' outlines and splines' curves drawn
    bool dragging() const { return drag_.active; }
    float3 viewPosition() const { return camPos_; }  // the camera the view was last drawn from
    // Glides the camera to the selected entity's bounds.
    void frameSelection();
    // Tests: pick at a fraction of the viewport, as a click there would.
    void pickAt(float fx, float fy);
    // The camera ray through a point of the view (fractions of it); false before the first frame.
    bool rayAt(float fx, float fy, float3& origin, float3& direction) const;
    // Editor code's view of it (the level API): the mouse, the camera's ray, what happened since
    // the last call. While captured, clicks, drags and keys go to editor code alone (a modal
    // operation in progress); the camera still flies.
    void codeInput(FxViewportState& out);
    void setCaptured(bool on)
    {
        if (on && !captured_) {  // (what started it, the click or key that ran the command, is not its input)
            codePressed_ = codeReleased_ = 0;
            codeKeys_.clear();
        }
        captured_ = on;
        if (!on)
            capturedWheel_ = false;
    }
    void setCapturedWheel(bool on) { capturedWheel_ = on; }  // (and the wheel: not a zoom while it holds)
    // The mouse as fractions of the view (the page's directional wheels open there); false: outside.
    // The ruler's ends (false: none), its length.
    bool measured(float3& a, float3& b) const
    {
        a = measure_[0];
        b = measure_[1];
        return measured_;
    }
    bool mouseFraction(float& fx, float& fy) const
    {
        fx = viewport_.w ? mouse_.x / float(viewport_.w) : 0.5f;
        fy = viewport_.h ? mouse_.y / float(viewport_.h) : 0.5f;
        return mouseInside_;
    }
    bool captured() const { return captured_; }
    // Tests: the transform gizmo's centre in window coordinates (as SDL events take them), offset
    // by (dx, dy) view pixels. False: no gizmo.
    bool gizmoWindowPoint(float dx, float dy, float& x, float& y) const;
    bool worldWindowPoint(float3 p, float dx, float dy, float& x, float& y) const;  // (tests: a point in window coordinates)

private:
    enum class Handle : uint32_t { None, X, Y, Z, PlaneYZ, PlaneXZ, PlaneXY, Center, ViewRing };
    struct Vertex {
        float x, y, z;
        uint32_t color;
    };
    struct Gizmo {
        bool valid = false;
        float3 origin, axis[3];  // pivot and unit axes
        float size = 1.0f;       // world length of an axis
    };
    struct Drag {
        bool active = false;
        bool elements = false;  // the selected elements of a level mesh, not entities
        // Entities moved with it (the selection's roots) and their worlds at the start.
        std::vector<std::pair<uint32_t, std::array<float, 12>>> roots;
        std::shared_ptr<const level::PolyMesh> mesh0;  // elements: the mesh at the start
        std::vector<float3> snapPoints;                // V: points to snap to (found at the start)
        Handle handle = Handle::None;
        uint32_t entity = ~0u;    // EditorBridge entity index
        float world0[3][4] = {};  // at the start (undo state, and the base of every update)
        Gizmo gizmo0;
        float3 hit0 = {};
        float param0 = 0.0f;
        float2 mouse0 = {};
    };

    bool project(float3 p, float2& pixel) const;
    float3 rayDirection(float2 pixel) const;
    Gizmo buildGizmo() const;
    Handle hitGizmo(float2 mouse) const;
    int hitOrientation(float2 mouse) const;  // axis index 0..5 (+X +Y +Z -X -Y -Z), -1 none
    uint32_t hitMarker(float2 mouse) const;  // the icon under the mouse (entity index), ~0u none
    // Drawn and clickable: everything but a damage cut whose mesh (or itself) isn't selected (a
    // damaged wall would be lost under them).
    bool markerShown(const EntityMarker& mk) const;
    bool outlineShown(const EntityMarker& mk) const;   // a volume's outline, a spline's curve
    bool nearSelection(const EntityMarker& mk) const;  // it, its parent or a child selected
    void beginDrag(Handle h, float2 mouse);
    void updateDrag(float2 mouse, bool snap);
    void endDrag(bool cancel);
    // Snapping a move (V: the nearest vertex or edge on screen; Ctrl+Shift: the surface under the
    // mouse): where the pivot goes. False: nothing near.
    bool snapTarget(float2 mouse, bool surface, float3& target) const;

    // Elements of the selected level mesh (scene_elements.cpp).
    bool elementTarget(uint32_t& index, float world[3][4]) const;  // the level mesh elements apply to
    bool elementCenter(float3& center) const;                      // the selected elements' centre (world)
    void elementClick(float2 mouse, uint32_t mode);                 // (mode: EditorBridge::SelectMode)
    void elementBox(float2 a, float2 b, uint32_t mode);
    void elementsMoved(const float delta[3][4], bool commit, bool cancel);  // a gizmo drag of them
    void buildElements();  // their overlay
    void boxSelect(float2 a, float2 b, uint32_t mode);
    // Geometry (framebuffer pixels -> NDC of the viewport).
    void line(float2 a, float2 b, float width, uint32_t color);
    void quad(float2 a, float2 b, float2 c, float2 d, uint32_t color);
    void disc(float2 center, float radius, uint32_t color, int segments = 16);
    void buildGeometry();
    void buildWorldLines();
    std::vector<WorldLine> worldLines_;
    std::vector<WorldLine> previewLines_;

    SDL_GPUDevice* device_ = nullptr;
    GpuDrivenRenderer* renderer_ = nullptr;
    ScenePak* pak_ = nullptr;
    FlyCamera* camera_ = nullptr;
    EditorBridge* bridge_ = nullptr;
    SDL_GPUGraphicsPipeline* overlay_ = nullptr;
    SDL_GPUGraphicsPipeline* color_ = nullptr;
    SDL_GPUBuffer* vertexBuffer_ = nullptr;
    SDL_GPUTransferBuffer* vertexUpload_ = nullptr;
    SDL_GPUTextureFormat format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    static constexpr uint32_t kMaxVertices = 1u << 15;

    SceneToolState tools_;
    UiRect viewport_ = {};
    float3 camPos_ = {}, camRight_ = {}, camUp_ = {}, camForward_ = {};
    float p00_ = 1.0f, p11_ = 1.0f, zNear_ = 0.05f;
    float2 mouse_ = {-1e9f, -1e9f};
    float pixelScaleX_ = 1.0f, pixelScaleY_ = 1.0f;  // window -> framebuffer (the last event's)
    // What editor code has not seen yet (codeInput), and whether it holds the view.
    uint32_t codeButtons_ = 0, codePressed_ = 0, codeReleased_ = 0;
    float codeWheel_ = 0.0f;
    std::vector<uint32_t> codeKeys_;
    bool captured_ = false;
    bool capturedWheel_ = false;
    bool mouseInside_ = false;
    Handle hover_ = Handle::None;
    int hoverAxis_ = -1;
    Drag drag_;
    bool clickPending_ = false;
    float2 clickAt_ = {};
    uint32_t clickMode_ = 0;  // EditorBridge::SelectMode of the click (Shift: add, Ctrl: toggle)
    uint32_t pickMode_ = 0;   // and of the pick it asked for
    bool marquee_ = false;    // the click became a box
    // Brush tool: a stroke in progress, and the terrain point under the mouse (decal).
    bool brushing_ = false, brushHover_ = false;
    bool painting_ = false;  // a level paint stroke (SceneTool::Paint)
    bool splining_ = false;  // a spline point being dragged (SceneTool::Spline)
    bool measuring_ = false;  // the ruler being dragged (SceneTool::Measure)
    float3 measure_[2] = {};
    bool measured_ = false;   // (a ruler to draw)
    bool measurePoint(float2 mouse, float3& out) const;
    int32_t splineHover_ = -1;  // the selected spline's point under the mouse
    int32_t splinePointAt(float2 mouse) const;
    float3 brushAt_ = {};
    bool brushHit(float2 mouse, float3& p) const;
    std::vector<Vertex> vertices_;
    std::vector<EntityMarker> markers_;  // mesh-less entities this frame (icons)
};

}  // namespace fx
