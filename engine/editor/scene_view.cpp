#include "editor/scene_view.h"

#include "assets/scene_pak.h"
#include "core/log.h"
#include "core/profile.h"
#include "audio/audio_bake.h"
#include "editor/editor_bridge.h"
#include "editor/terrain_editor.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "level/mesh_cut.h"
#include "render/gpu_driven_renderer.h"
#include "scene/fly_camera.h"
#include "water/water_system.h"

#include "editor_scene_view.shader.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

constexpr float kGizmoPixels = 105.0f;     // on-screen length of an axis
constexpr float kPickPixels = 8.0f;        // hit distance to a handle
constexpr float kOrientRadius = 38.0f;     // orientation gizmo: axis length in pixels
constexpr float kOrientMargin = 66.0f;     // its center from the top-right corner

constexpr uint32_t rgba(float r, float g, float b, float a = 1.0f)
{
    return uint32_t(r * 255.0f + 0.5f) | uint32_t(g * 255.0f + 0.5f) << 8 | uint32_t(b * 255.0f + 0.5f) << 16 |
           uint32_t(a * 255.0f + 0.5f) << 24;
}
// Unity's handle colors.
constexpr uint32_t kAxisColor[3] = {rgba(0.86f, 0.24f, 0.20f), rgba(0.55f, 0.80f, 0.24f), rgba(0.22f, 0.47f, 0.93f)};
constexpr uint32_t kActive = rgba(0.98f, 0.86f, 0.18f);
constexpr uint32_t kViewRing = rgba(0.85f, 0.85f, 0.85f, 0.85f);
constexpr uint32_t kCenter = rgba(0.85f, 0.85f, 0.85f, 0.9f);

uint32_t withAlpha(uint32_t c, float a) { return (c & 0x00ffffffu) | uint32_t(std::clamp(a, 0.0f, 1.0f) * 255.0f) << 24; }

float2 operator-(float2 a, float2 b) { return {a.x - b.x, a.y - b.y}; }
float2 operator+(float2 a, float2 b) { return {a.x + b.x, a.y + b.y}; }
float2 operator*(float2 a, float s) { return {a.x * s, a.y * s}; }
float len2(float2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }

float segmentDistance(float2 p, float2 a, float2 b)
{
    const float2 ab = b - a, ap = p - a;
    const float l = ab.x * ab.x + ab.y * ab.y;
    const float t = l > 0.0f ? std::clamp((ap.x * ab.x + ap.y * ab.y) / l, 0.0f, 1.0f) : 0.0f;
    return len2(p - (a + ab * t));
}

bool insideQuad(float2 p, const float2 q[4])
{
    // Same sign of the cross product along every edge (either winding).
    int sign = 0;
    for (int i = 0; i < 4; ++i) {
        const float2 e = q[(i + 1) % 4] - q[i], v = p - q[i];
        const float c = e.x * v.y - e.y * v.x;
        const int s = c > 0.0f ? 1 : c < 0.0f ? -1 : 0;
        if (s != 0 && sign != 0 && s != sign)
            return false;
        if (s != 0)
            sign = s;
    }
    return true;
}

// Closest point on the line o + a t to the ray c + d s: returns t.
float closestOnAxis(float3 o, float3 a, float3 c, float3 d)
{
    const float3 w = o - c;
    const float b = dot(a, d), dd = dot(d, d), aa = dot(a, a);
    const float denom = aa * dd - b * b;
    if (std::fabs(denom) < 1e-8f)
        return 0.0f;
    return (b * dot(d, w) - dd * dot(a, w)) / denom;
}

bool rayPlane(float3 c, float3 d, float3 o, float3 n, float3& hit)
{
    const float dn = dot(d, n);
    if (std::fabs(dn) < 1e-6f)
        return false;
    const float t = dot(o - c, n) / dn;
    if (t <= 0.0f)
        return false;
    hit = c + d * t;
    return true;
}

// Rotation by `angle` about unit axis `a`, applied to v (Rodrigues).
float3 rotate(float3 v, float3 a, float angle)
{
    const float c = std::cos(angle), s = std::sin(angle);
    return v * c + cross(a, v) * s + a * (dot(a, v) * (1.0f - c));
}

float snapTo(float v, float step) { return step > 0.0f ? std::round(v / step) * step : v; }

// Seven-segment-ish strokes for the orientation gizmo's labels (unit box, y up).
struct Stroke {
    float x0, y0, x1, y1;
};
constexpr Stroke kLetterX[] = {{0, 0, 1, 1}, {0, 1, 1, 0}};
constexpr Stroke kLetterY[] = {{0, 1, 0.5f, 0.5f}, {1, 1, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f, 0}};
constexpr Stroke kLetterZ[] = {{0, 1, 1, 1}, {1, 1, 0, 0}, {0, 0, 1, 0}};

}  // namespace

bool SceneView::init(const GpuDevice& gpu, GpuDrivenRenderer& renderer, ScenePak& pak, FlyCamera& camera,
                     EditorBridge& bridge)
{
    FX_ZONE_CAT("SceneView::init", Editor);
    device_ = gpu.get();
    renderer_ = &renderer;
    pak_ = &pak;
    camera_ = &camera;
    bridge_ = &bridge;
    format_ = gpu.swapchainFormat();

    SDL_GPUShader* fullscreen = gpu.createShader(shaders::editor_scene_view_vs_fullscreen);
    SDL_GPUShader* overlay = gpu.createShader(shaders::editor_scene_view_ps_overlay);
    SDL_GPUShader* vs = gpu.createShader(shaders::editor_scene_view_vs_color);
    SDL_GPUShader* fs = gpu.createShader(shaders::editor_scene_view_ps_color);
    SDL_GPUColorTargetDescription color = {};
    color.format = format_;
    color.blend_state.enable_blend = true;
    color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    info.vertex_shader = fullscreen;
    info.fragment_shader = overlay;
    overlay_ = fullscreen && overlay ? SDL_CreateGPUGraphicsPipeline(device_, &info) : nullptr;
    info.vertex_shader = vs;
    info.fragment_shader = fs;
    color_ = vs && fs ? SDL_CreateGPUGraphicsPipeline(device_, &info) : nullptr;
    for (SDL_GPUShader* s : {fullscreen, overlay, vs, fs})
        if (s)
            SDL_ReleaseGPUShader(device_, s);

    SDL_GPUBufferCreateInfo vb = {SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, kMaxVertices * sizeof(Vertex), 0};
    vertexBuffer_ = gpumem::createBuffer("Editor", device_, &vb);
    SDL_GPUTransferBufferCreateInfo tb = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, kMaxVertices * sizeof(Vertex), 0};
    vertexUpload_ = gpumem::createTransferBuffer("Editor", device_, &tb);
    if (!overlay_ || !color_ || !vertexBuffer_ || !vertexUpload_) {
        FX_ERROR("scene view: %s", SDL_GetError());
        return false;
    }
    vertices_.reserve(4096);
    return true;
}

void SceneView::shutdown()
{
    if (!device_)
        return;
    SDL_ReleaseGPUGraphicsPipeline(device_, overlay_);
    SDL_ReleaseGPUGraphicsPipeline(device_, color_);
    gpumem::releaseBuffer(device_, vertexBuffer_);
    gpumem::releaseTransferBuffer(device_, vertexUpload_);
    device_ = nullptr;
}

// ---- camera math (viewport pixels)

bool SceneView::project(float3 p, float2& pixel) const
{
    const float3 v = p - camPos_;
    const float z = dot(v, camForward_);
    if (z <= zNear_)
        return false;
    const float sx = dot(v, camRight_) * p00_ / z, sy = dot(v, camUp_) * p11_ / z;
    pixel = {(sx + 1.0f) * 0.5f * float(viewport_.w), (1.0f - sy) * 0.5f * float(viewport_.h)};
    return true;
}

bool SceneView::rayAt(float fx, float fy, float3& origin, float3& direction) const
{
    if (viewport_.w == 0 || viewport_.h == 0 || p00_ == 0.0f)
        return false;
    origin = camPos_;
    direction = normalize(rayDirection({fx * float(viewport_.w), fy * float(viewport_.h)}));
    return true;
}

float3 SceneView::rayDirection(float2 pixel) const
{
    const float nx = pixel.x / float(viewport_.w) * 2.0f - 1.0f, ny = 1.0f - pixel.y / float(viewport_.h) * 2.0f;
    return camForward_ + camRight_ * (nx / p00_) + camUp_ * (ny / p11_);
}

SceneView::Gizmo SceneView::buildGizmo() const
{
    Gizmo g;
    const uint32_t entity = bridge_->selected();
    float w[3][4];
    // (none while editor code holds the view: a modal operation's, a building tool's ghost selected)
    if (captured_ || entity == ~0u || !bridge_->worldOf(entity, w) || tools_.tool == SceneTool::View || tools_.tool == SceneTool::Brush ||
        tools_.tool == SceneTool::Paint || tools_.tool == SceneTool::Spline || tools_.tool == SceneTool::Measure)
        return g;
    g.origin = {w[0][3], w[1][3], w[2][3]};
    float3 c, half;
    if (tools_.element != ElementMode::Object && bridge_->levelMeshOf(entity)) {
        if (!elementCenter(g.origin))
            return g;  // (an element mode with nothing chosen: no gizmo)
    } else if (tools_.pivotCenter && bridge_->meshBounds(entity, c, half)) {
        for (int r = 0; r < 3; ++r)
            (&g.origin.x)[r] = w[r][0] * c.x + w[r][1] * c.y + w[r][2] * c.z + w[r][3];
    }
    // Scale handles always follow the entity's own axes (as in Unity).
    const bool local = tools_.local || tools_.tool == SceneTool::Scale;
    for (int a = 0; a < 3; ++a) {
        const float3 col = {w[0][a], w[1][a], w[2][a]};
        g.axis[a] = local && length(col) > 1e-8f ? normalize(col) : float3{a == 0 ? 1.0f : 0.0f, a == 1 ? 1.0f : 0.0f, a == 2 ? 1.0f : 0.0f};
    }
    const float z = dot(g.origin - camPos_, camForward_);
    if (z <= zNear_)
        return g;
    g.size = kGizmoPixels * 2.0f * z / (p11_ * float(viewport_.h));
    g.valid = true;
    return g;
}

// ---- hit testing

bool SceneView::nearSelection(const EntityMarker& mk) const
{
    for (const uint32_t k : bridge_->selection())
        if (k == mk.index || (mk.parent != ~0u && k == mk.parent) || bridge_->parentOf(k) == mk.index)
            return true;
    return false;
}

bool SceneView::markerShown(const EntityMarker& mk) const
{
    // A cut volume (a doorway, a damage cut) only with it, what it is under (its wall) or what is under
    // it (a door's leaf) selected: not over every wall otherwise.
    return !mk.cutShape || nearSelection(mk);
}

bool SceneView::outlineShown(const EntityMarker& mk) const
{
    // A gameplay volume's outline and a spline's curve the same way (a road's curve not over the
    // road, a trigger's box not over the level); every spline's while the Spline tool is on.
    return nearSelection(mk) || (!mk.splineCurve.empty() && tools_.tool == SceneTool::Spline);
}

uint32_t SceneView::damageShown() const
{
    uint32_t n = 0;
    for (const EntityMarker& mk : markers_)
        n += mk.cutShape == uint32_t(level::CutShape::Damage) + 1 && markerShown(mk);
    return n;
}

uint32_t SceneView::cutsShown() const
{
    uint32_t n = 0;
    for (const EntityMarker& mk : markers_)
        n += mk.cutShape && markerShown(mk);
    return n;
}

uint32_t SceneView::outlinesShown() const
{
    uint32_t n = 0;
    for (const EntityMarker& mk : markers_)
        n += (mk.volumeKind || !mk.splineCurve.empty()) && outlineShown(mk);
    return n;
}

uint32_t SceneView::hitMarker(float2 mouse) const
{
    uint32_t best = ~0u;
    float bestDistance = 12.0f;  // pixels
    for (const EntityMarker& mk : markers_) {
        float2 p;
        if (!markerShown(mk) || !project({mk.world[0][3], mk.world[1][3], mk.world[2][3]}, p))
            continue;
        if (const float d = len2(mouse - p); d < bestDistance) {
            bestDistance = d;
            best = mk.index;
        }
    }
    for (const EntityMarker& mk : markers_)  // (a spline: anywhere along its curve; a volume: its outline)
        for (size_t k = 0; outlineShown(mk) && k + 1 < mk.splineCurve.size() + mk.volumeEdges.size(); k += mk.volumeEdges.empty() ? 1 : 2) {
            const std::vector<float3>& line = mk.volumeEdges.empty() ? mk.splineCurve : mk.volumeEdges;
            if (k + 1 >= line.size())
                break;
            float2 a, b;
            if (!project(line[k], a) || !project(line[k + 1], b))
                continue;
            const float2 ab = b - a;
            const float t = std::clamp(((mouse.x - a.x) * ab.x + (mouse.y - a.y) * ab.y) / std::max(ab.x * ab.x + ab.y * ab.y, 1e-6f), 0.0f, 1.0f);
            if (const float d = len2(mouse - (a + ab * t)); d < std::min(bestDistance, 6.0f)) {
                bestDistance = d;
                best = mk.index;
            }
        }
    return best;
}

SceneView::Handle SceneView::hitGizmo(float2 mouse) const
{
    const Gizmo g = drag_.active ? drag_.gizmo0 : buildGizmo();
    float2 o;
    if (!g.valid || !project(g.origin, o))
        return Handle::None;
    Handle best = Handle::None;
    float bestDist = kPickPixels;
    const float3 view = normalize(g.origin - camPos_);
    const Handle axisHandle[3] = {Handle::X, Handle::Y, Handle::Z};
    if (tools_.tool == SceneTool::Rotate) {
        if (std::fabs(len2(mouse - o) - kGizmoPixels * 1.15f) < bestDist)
            best = Handle::ViewRing, bestDist = std::fabs(len2(mouse - o) - kGizmoPixels * 1.15f);
        for (int a = 0; a < 3; ++a) {
            const float3 u = normalize(cross(g.axis[a], std::fabs(g.axis[a].y) < 0.9f ? float3{0, 1, 0} : float3{1, 0, 0}));
            const float3 v = cross(g.axis[a], u);
            float2 prev;
            bool havePrev = false;
            for (int k = 0; k <= 64; ++k) {
                const float t = float(k) / 64.0f * 2.0f * kPi;
                const float3 p = g.origin + (u * std::cos(t) + v * std::sin(t)) * g.size;
                // Only the front half of each ring is grabbable (the back half is behind the ball).
                float2 s;
                const bool front = dot(p - g.origin, view) <= 0.0f;
                if (!project(p, s)) {
                    havePrev = false;
                    continue;
                }
                if (havePrev && front) {
                    const float d = segmentDistance(mouse, prev, s);
                    if (d < bestDist)
                        best = axisHandle[a], bestDist = d;
                }
                prev = s;
                havePrev = true;
            }
        }
        return best;
    }
    if (len2(mouse - o) < kPickPixels * 1.2f)
        return Handle::Center;
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(dot(g.axis[a], view)) > 0.97f)
            continue;  // pointing at the camera
        float2 e;
        if (!project(g.origin + g.axis[a] * g.size, e))
            continue;
        const float d = segmentDistance(mouse, o, e);
        if (d < bestDist)
            best = axisHandle[a], bestDist = d;
    }
    if (best != Handle::None || tools_.tool != SceneTool::Move)
        return best;
    // Plane handles: small squares between two axes.
    const Handle planes[3] = {Handle::PlaneYZ, Handle::PlaneXZ, Handle::PlaneXY};
    for (int n = 0; n < 3; ++n) {
        const float3 a = g.axis[(n + 1) % 3], b = g.axis[(n + 2) % 3];
        float2 q[4];
        const float lo = g.size * 0.15f, hi = g.size * 0.4f;
        if (project(g.origin + a * lo + b * lo, q[0]) && project(g.origin + a * hi + b * lo, q[1]) &&
            project(g.origin + a * hi + b * hi, q[2]) && project(g.origin + a * lo + b * hi, q[3]) && insideQuad(mouse, q))
            return planes[n];
    }
    return Handle::None;
}

int SceneView::hitOrientation(float2 mouse) const
{
    const float2 c = {float(viewport_.w) - kOrientMargin, kOrientMargin};
    int best = -1;
    float bestDist = 12.0f;
    for (int k = 0; k < 6; ++k) {
        const float s = k < 3 ? 1.0f : -1.0f;
        const float3 d = {k % 3 == 0 ? s : 0.0f, k % 3 == 1 ? s : 0.0f, k % 3 == 2 ? s : 0.0f};
        const float2 p = c + float2{dot(d, camRight_), -dot(d, camUp_)} * kOrientRadius;
        const float dist = len2(mouse - p);
        if (dist < bestDist && dot(d, camForward_) < 0.3f)  // axes pointing away are behind the others
            best = k, bestDist = dist;
    }
    return best;
}

// ---- dragging

void SceneView::beginDrag(Handle h, float2 mouse)
{
    drag_ = {};
    drag_.entity = bridge_->selected();
    if (drag_.entity == ~0u || !bridge_->worldOf(drag_.entity, drag_.world0))
        return;
    drag_.gizmo0 = buildGizmo();
    if (!drag_.gizmo0.valid)
        return;
    std::vector<uint32_t> skip;
    if (tools_.element != ElementMode::Object && bridge_->levelMeshOf(drag_.entity)) {
        // The selected elements: their frame is the gizmo's (its axes at their centre).
        drag_.elements = true;
        drag_.mesh0 = bridge_->levelMeshOf(drag_.entity);
        const Gizmo& g = drag_.gizmo0;
        for (int r = 0; r < 3; ++r) {
            for (int a = 0; a < 3; ++a)
                drag_.world0[r][a] = (&g.axis[a].x)[r];
            drag_.world0[r][3] = (&g.origin.x)[r];
        }
    } else {
        for (const uint32_t r : bridge_->selectionRoots())
            if (std::array<float, 12> w; bridge_->worldOf(r, reinterpret_cast<float(*)[4]>(w.data()))) {
                drag_.roots.push_back({r, w});
                skip.push_back(r);
            }
    }
    if (drag_.elements)
        skip.push_back(drag_.entity);
    if (tools_.tool == SceneTool::Move)
        bridge_->snapPoints(skip, drag_.snapPoints);
    drag_.active = true;
    drag_.handle = h;
    drag_.mouse0 = mouse;
    const Gizmo& g = drag_.gizmo0;
    const float3 dir = rayDirection(mouse);
    const int axis = h == Handle::X ? 0 : h == Handle::Y ? 1 : h == Handle::Z ? 2 : -1;
    if (axis >= 0 && tools_.tool != SceneTool::Rotate)
        drag_.param0 = closestOnAxis(g.origin, g.axis[axis], camPos_, dir);
    const float3 normal = axis >= 0                       ? g.axis[axis]
                          : h == Handle::PlaneYZ          ? g.axis[0]
                          : h == Handle::PlaneXZ          ? g.axis[1]
                          : h == Handle::PlaneXY          ? g.axis[2]
                                                          : camForward_;
    if (!rayPlane(camPos_, dir, g.origin, normal, drag_.hit0))
        drag_.hit0 = g.origin;
}

void SceneView::updateDrag(float2 mouse, bool snap)
{
    const Gizmo& g = drag_.gizmo0;
    const float3 dir = rayDirection(mouse);
    const int axis = drag_.handle == Handle::X ? 0 : drag_.handle == Handle::Y ? 1 : drag_.handle == Handle::Z ? 2 : -1;
    float w[3][4];
    std::memcpy(w, drag_.world0, sizeof(w));
    const float3 origin0 = {w[0][3], w[1][3], w[2][3]};
    auto setOrigin = [&](float3 o) {
        w[0][3] = o.x;
        w[1][3] = o.y;
        w[2][3] = o.z;
    };

    const SDL_Keymod mods = SDL_GetModState();
    const bool* keys = SDL_GetKeyboardState(nullptr);
    float3 snapped;
    if (tools_.tool == SceneTool::Move && keys && keys[SDL_SCANCODE_V] && snapTarget(mouse, false, snapped)) {
        setOrigin(origin0 + (snapped - g.origin));  // the pivot onto the nearest vertex or edge middle
    } else if (tools_.tool == SceneTool::Move && snap && (mods & SDL_KMOD_SHIFT) && snapTarget(mouse, true, snapped)) {
        setOrigin(origin0 + (snapped - g.origin));  // onto the surface under the mouse
    } else if (tools_.tool == SceneTool::Move) {
        float3 delta = {0, 0, 0};
        if (axis >= 0) {
            float t = closestOnAxis(g.origin, g.axis[axis], camPos_, dir) - drag_.param0;
            if (snap && tools_.snapAbsolute && !tools_.local)  // the pivot onto a grid line of that axis
                t = snapTo(dot(g.origin, g.axis[axis]) + t, tools_.snapMove) - dot(g.origin, g.axis[axis]);
            else if (snap)
                t = snapTo(t, tools_.snapMove);
            delta = g.axis[axis] * t;
        } else {
            const float3 normal = drag_.handle == Handle::PlaneYZ   ? g.axis[0]
                                  : drag_.handle == Handle::PlaneXZ ? g.axis[1]
                                  : drag_.handle == Handle::PlaneXY ? g.axis[2]
                                                                    : camForward_;
            float3 hit;
            if (!rayPlane(camPos_, dir, g.origin, normal, hit))
                return;
            delta = hit - drag_.hit0;
            if (snap && tools_.snapAbsolute && !tools_.local) {  // the pivot onto grid points (the moved axes)
                const float3 to = g.origin + delta;
                for (int a = 0; a < 3; ++a)
                    if (std::fabs(dot(delta, g.axis[a])) > 1e-6f || drag_.handle == Handle::Center) {
                        const float c = dot(to, g.axis[a]);
                        delta = delta + g.axis[a] * (snapTo(c, tools_.snapMove) - c);
                    }
            } else if (snap) {  // per gizmo axis
                for (int a = 0; a < 3; ++a) {
                    const float c = dot(delta, g.axis[a]);
                    delta = delta + g.axis[a] * (snapTo(c, tools_.snapMove) - c);
                }
            }
        }
        setOrigin(origin0 + delta);
    } else if (tools_.tool == SceneTool::Rotate) {
        float3 rotAxis = axis >= 0 ? g.axis[axis] : camForward_ * -1.0f;
        float angle = 0.0f;
        float3 hit;
        float2 o;
        if (axis >= 0 && rayPlane(camPos_, dir, g.origin, rotAxis, hit) &&
            std::fabs(dot(normalize(dir), rotAxis)) > 0.08f) {
            const float3 v0 = drag_.hit0 - g.origin, v1 = hit - g.origin;
            angle = std::atan2(dot(cross(v0, v1), rotAxis), dot(v0, v1));
        } else if (project(g.origin, o)) {
            // View ring, or a ring seen edge-on: angle swept around the gizmo on screen.
            const float2 a0 = drag_.mouse0 - o, a1 = mouse - o;
            angle = std::atan2(a0.x * a1.y - a0.y * a1.x, a0.x * a1.x + a0.y * a1.y);
            if (axis >= 0)
                angle *= dot(rotAxis, camForward_) > 0.0f ? 1.0f : -1.0f;
            else
                rotAxis = camForward_;
        }
        if (snap)
            angle = snapTo(angle * 180.0f / kPi, tools_.snapRotate) * kPi / 180.0f;
        for (int c = 0; c < 3; ++c) {
            const float3 col = rotate({w[0][c], w[1][c], w[2][c]}, rotAxis, angle);
            w[0][c] = col.x;
            w[1][c] = col.y;
            w[2][c] = col.z;
        }
        setOrigin(g.origin + rotate(origin0 - g.origin, rotAxis, angle));
    } else if (tools_.tool == SceneTool::Scale) {
        float factor[3] = {1, 1, 1};
        if (axis >= 0) {
            const float t = closestOnAxis(g.origin, g.axis[axis], camPos_, dir);
            float f = std::fabs(drag_.param0) > 1e-6f ? t / drag_.param0 : 1.0f;
            if (snap)
                f = std::max(snapTo(f, tools_.snapScale), tools_.snapScale);
            factor[axis] = std::max(f, 0.001f);
        } else {
            float f = std::max(1.0f + ((mouse.x - drag_.mouse0.x) - (mouse.y - drag_.mouse0.y)) / 120.0f, 0.001f);
            if (snap)
                f = std::max(snapTo(f, tools_.snapScale), tools_.snapScale);
            factor[0] = factor[1] = factor[2] = f;
        }
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                w[r][c] *= factor[c];
        if (tools_.pivotCenter) {  // keep the bounds' center in place
            float3 local = origin0 - g.origin;
            float3 scaled = {0, 0, 0};
            for (int a = 0; a < 3; ++a)
                scaled = scaled + g.axis[a] * (dot(local, g.axis[a]) * factor[a]);
            setOrigin(g.origin + scaled);
        }
    }
    // The drag's change (new pivot frame times the old one's inverse), to everything it moves.
    float inv[3][4], change[3][4];
    affineInverse(drag_.world0, inv);
    affineMultiply(w, inv, change);
    if (drag_.elements)
        return elementsMoved(change, false, false);
    if (drag_.roots.empty())
        return bridge_->setWorldLive(drag_.entity, w);
    for (const auto& [index, world0] : drag_.roots) {
        float moved[3][4];
        affineMultiply(change, reinterpret_cast<const float(*)[4]>(world0.data()), moved);
        if (tools_.tool == SceneTool::Move && drag_.roots.size() == 1)  // (one kit piece near another's snap point: onto it)
            bridge_->snapPiece(index, moved);
        bridge_->setWorldLive(index, moved);
    }
}

void SceneView::endDrag(bool cancel)
{
    if (!drag_.active)
        return;
    if (drag_.elements) {
        elementsMoved(drag_.world0, !cancel, cancel);
    } else if (drag_.roots.empty()) {
        if (cancel)
            bridge_->setWorldLive(drag_.entity, drag_.world0);
        else
            bridge_->commitWorld(drag_.entity, drag_.world0);
    } else if (cancel) {
        for (const auto& [index, world0] : drag_.roots)
            bridge_->setWorldLive(index, reinterpret_cast<const float(*)[4]>(world0.data()));
    } else {
        bridge_->commitWorlds(drag_.roots);
    }
    drag_ = {};
}

void SceneView::codeInput(FxViewportState& out)
{
    out = {};
    out.mouse[0] = mouse_.x;
    out.mouse[1] = mouse_.y;
    out.size[0] = float(viewport_.w);
    out.size[1] = float(viewport_.h);
    const float3 d = normalize(rayDirection(mouse_));
    for (int a = 0; a < 3; ++a) {
        out.rayOrigin[a] = (&camPos_.x)[a];
        out.rayDir[a] = (&d.x)[a];
        out.cameraPos[a] = (&camPos_.x)[a];
        out.cameraForward[a] = (&camForward_.x)[a];
        out.cameraRight[a] = (&camRight_.x)[a];
        out.cameraUp[a] = (&camUp_.x)[a];
    }
    out.p00 = p00_;
    out.p11 = p11_;
    out.buttons = codeButtons_;
    out.pressed = codePressed_;
    out.released = codeReleased_;
    const SDL_Keymod mod = SDL_GetModState();
    out.mods = ((mod & SDL_KMOD_SHIFT) ? 1u : 0u) | ((mod & SDL_KMOD_CTRL) ? 2u : 0u) | ((mod & SDL_KMOD_ALT) ? 4u : 0u);
    out.wheel = codeWheel_;
    out.inside = mouseInside_ ? 1u : 0u;
    out.captured = captured_ ? 1u : 0u;
    out.elementMode = uint32_t(tools_.element);
    out.keyCount = int32_t(std::min<size_t>(codeKeys_.size(), 16));
    for (int32_t k = 0; k < out.keyCount; ++k)
        out.keys[k] = codeKeys_[size_t(k)];
    codePressed_ = codeReleased_ = 0;
    codeWheel_ = 0.0f;
    codeKeys_.clear();
}

bool SceneView::worldWindowPoint(float3 p, float dx, float dy, float& x, float& y) const
{
    float2 q;
    if (!project(p, q))
        return false;
    x = (q.x + dx + float(viewport_.x)) / pixelScaleX_;
    y = (q.y + dy + float(viewport_.y)) / pixelScaleY_;
    return true;
}

bool SceneView::measurePoint(float2 mouse, float3& out) const
{
    // A surface's point under the mouse (V held: the nearest vertex or edge middle instead).
    const bool* keys = SDL_GetKeyboardState(nullptr);
    if (keys && keys[SDL_SCANCODE_V] && snapTarget(mouse, false, out))
        return true;
    float3 normal;
    return bridge_->surfaceHit(camPos_, rayDirection(mouse), {}, out, normal);
}

int32_t SceneView::splinePointAt(float2 mouse) const
{
    std::vector<float3> points;
    const uint32_t entity = bridge_->selected();
    if (entity == ~0u || !bridge_->splineWorld(entity, &points))
        return -1;
    int32_t best = -1;
    float bestDistance = 10.0f;  // pixels
    for (size_t k = 0; k < points.size(); ++k)
        if (float2 q; project(points[k], q))
            if (const float d = len2(mouse - q); d < bestDistance) {
                bestDistance = d;
                best = int32_t(k);
            }
    return best;
}

bool SceneView::gizmoWindowPoint(float dx, float dy, float& x, float& y) const
{
    const Gizmo g = buildGizmo();
    float2 p;
    if (!g.valid || !project(g.origin, p))
        return false;
    x = (p.x + dx + float(viewport_.x)) / pixelScaleX_;
    y = (p.y + dy + float(viewport_.y)) / pixelScaleY_;
    return true;
}

void SceneView::pickAt(float fx, float fy)
{
    if (renderer_ && viewport_.w > 0)
        renderer_->requestPick(uint32_t(fx * float(viewport_.w - 1)), uint32_t(fy * float(viewport_.h - 1)));
}

void SceneView::frameSelection()
{
    const uint32_t entity = bridge_->selected();
    float w[3][4];
    if (entity == ~0u || !bridge_->worldOf(entity, w))
        return;
    float3 c = {}, half = {0.5f, 0.5f, 0.5f};  // without a mesh: about a unit around it
    bridge_->meshBounds(entity, c, half);
    float3 center;
    for (int r = 0; r < 3; ++r)
        (&center.x)[r] = w[r][0] * c.x + w[r][1] * c.y + w[r][2] * c.z + w[r][3];
    float scale = 0.0f;
    for (int a = 0; a < 3; ++a)
        scale = std::max(scale, length(float3{w[0][a], w[1][a], w[2][a]}));
    camera_->frameOn(center, length(half) * scale);
}

bool SceneView::brushHit(float2 mouse, float3& p) const
{
    const TerrainEditor* terrain = bridge_->terrainEditor();
    return terrain && terrain->hit(camPos_, rayDirection(mouse), p);
}

// ---- events

bool SceneView::handleEvent(const SDL_Event& e, float pixelScaleX, float pixelScaleY)
{
    if (!device_)  // before the UI (and with it the scene view) started
        return false;
    auto toViewport = [&](float x, float y) {
        return float2{x * pixelScaleX - float(viewport_.x), y * pixelScaleY - float(viewport_.y)};
    };
    auto inside = [&](float2 p) { return p.x >= 0 && p.y >= 0 && p.x < float(viewport_.w) && p.y < float(viewport_.h); };
    pixelScaleX_ = pixelScaleX;
    pixelScaleY_ = pixelScaleY;
    // Everything editor code may want (the level API's viewport): buttons, the wheel, keys.
    const auto buttonBit = [](uint8_t b) { return b == SDL_BUTTON_LEFT ? 1u : b == SDL_BUTTON_RIGHT ? 2u : b == SDL_BUTTON_MIDDLE ? 4u : 0u; };
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && (inside(toViewport(e.button.x, e.button.y)) || captured_)) {
        codeButtons_ |= buttonBit(e.button.button);
        codePressed_ |= buttonBit(e.button.button);
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (codeButtons_ & buttonBit(e.button.button))
            codeReleased_ |= buttonBit(e.button.button);
        codeButtons_ &= ~buttonBit(e.button.button);
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL && mouseInside_) {
        codeWheel_ += e.wheel.y;
    } else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && (mouseInside_ || captured_) && codeKeys_.size() < 64) {
        codeKeys_.push_back(uint32_t(e.key.scancode));
    }
    // A modal operation of editor code holds the view: its clicks and keys are its own.
    if (captured_ && !tools_.gameView) {
        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            mouse_ = toViewport(e.motion.x, e.motion.y);
            mouseInside_ = inside(mouse_);
            return false;  // (the camera may still look around)
        }
        if ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) && e.button.button == SDL_BUTTON_LEFT)
            return true;
        if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP)
            return true;
        if (e.type == SDL_EVENT_MOUSE_WHEEL && capturedWheel_)
            return true;
    }
    const bool alt = (SDL_GetModState() & SDL_KMOD_ALT) != 0;
    TerrainEditor* terrain = bridge_->terrainEditor();
    const bool brushTool = tools_.tool == SceneTool::Brush && terrain && terrain->valid() && !tools_.gameView;
    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:
        mouse_ = toViewport(e.motion.x, e.motion.y);
        mouseInside_ = inside(mouse_);
        if (brushing_) {
            if (float3 p; brushHit(mouse_, p))
                terrain->strokeTo(p);
            return true;
        }
        if (painting_) {
            bridge_->paintTo(camPos_, rayDirection(mouse_));
            return true;
        }
        if (splining_) {
            bridge_->splineDragTo(camPos_, rayDirection(mouse_));
            return true;
        }
        if (measuring_) {
            if (float3 p; measurePoint(mouse_, p)) {
                measure_[1] = p;
                bridge_->measuredTo(measure_[0], measure_[1]);
            }
            return true;
        }
        if (drag_.active) {
            updateDrag(mouse_, (SDL_GetModState() & SDL_KMOD_CTRL) != 0);
            return true;
        }
        if (clickPending_ && !marquee_ && tools_.tool != SceneTool::Paint && len2(mouse_ - clickAt_) >= 4.0f)
            marquee_ = true;  // a box: everything inside it on release
        return clickPending_;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (e.button.button != SDL_BUTTON_LEFT || alt || camera_->busy() || tools_.gameView)
            return false;
        const float2 m = toViewport(e.button.x, e.button.y);
        if (!inside(m))
            return false;
        mouse_ = m;
        if (const int axis = hitOrientation(m); axis >= 0) {
            const float s = axis < 3 ? 1.0f : -1.0f;
            const float3 d = {axis % 3 == 0 ? s : 0.0f, axis % 3 == 1 ? s : 0.0f, axis % 3 == 2 ? s : 0.0f};
            camera_->lookAlong(d * -1.0f);  // view from that side
            return true;
        }
        if (tools_.tool == SceneTool::Paint) {  // level paint: the stroke starts on the selected level mesh
            bool missed = false;
            painting_ = bridge_->paintBegin(camPos_, rayDirection(m), &missed);
            if (missed) {  // (off it: a click picks what is there, to paint that; no box)
                clickMode_ = uint32_t(EditorBridge::SelectMode::Replace);
                clickPending_ = true;
                marquee_ = false;
                clickAt_ = m;
            }
            return true;
        }
        if (tools_.tool == SceneTool::Measure) {  // the ruler from here (dragged to its other end)
            if (float3 p; measurePoint(m, p)) {
                measure_[0] = measure_[1] = p;
                measuring_ = measured_ = true;
                bridge_->measuredTo(p, p);
            }
            return true;
        }
        if (tools_.tool == SceneTool::Spline) {
            // A point of the selected spline: dragged; else one added where the mouse is (Ctrl: put in
            // between the two it is nearest). (Alt is the camera's.)
            const SDL_Keymod mod = SDL_GetModState();
            if (const int32_t k = splinePointAt(m); k >= 0) {
                splining_ = bridge_->splineDragBegin(uint32_t(k));
            } else {
                bridge_->splineAdd(camPos_, rayDirection(m), (mod & SDL_KMOD_CTRL) != 0);
            }
            return true;
        }
        if (brushTool) {  // sculpt: the stroke starts where the mouse meets the terrain
            if (float3 p; brushHit(m, p)) {
                const SDL_Keymod mod = SDL_GetModState();
                terrain->beginStroke(p, (mod & SDL_KMOD_SHIFT) != 0, (mod & SDL_KMOD_CTRL) != 0);
                brushing_ = true;
            }
            return true;
        }
        if (const Handle h = hitGizmo(m); h != Handle::None) {
            beginDrag(h, m);
            return true;
        }
        const SDL_Keymod mod = SDL_GetModState();
        clickMode_ = (mod & SDL_KMOD_CTRL) ? uint32_t(EditorBridge::SelectMode::Toggle)
                     : (mod & SDL_KMOD_SHIFT) ? uint32_t(EditorBridge::SelectMode::Add)
                                              : uint32_t(EditorBridge::SelectMode::Replace);
        if (const uint32_t entity = hitMarker(m); entity != ~0u && tools_.element == ElementMode::Object) {  // icons win over the GPU pick
            if (clickMode_ == uint32_t(EditorBridge::SelectMode::Replace))
                bridge_->selectEntity(entity);
            else {
                bridge_->selectMode(entity, EditorBridge::SelectMode(clickMode_));
                bridge_->toolsChanged();
            }
            return true;
        }
        clickPending_ = true;
        marquee_ = false;
        clickAt_ = m;
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button != SDL_BUTTON_LEFT)
            return false;
        if (brushing_) {
            brushing_ = false;
            bridge_->terrainEdited(terrain ? terrain->endStroke() : ~0u);
            return true;
        }
        if (painting_) {
            painting_ = false;
            bridge_->paintEnd();
            return true;
        }
        if (splining_) {
            splining_ = false;
            bridge_->splineDragEnd(false);
            return true;
        }
        if (measuring_) {
            measuring_ = false;
            return true;
        }
        if (drag_.active) {
            endDrag(false);
            return true;
        }
        if (clickPending_) {
            clickPending_ = false;
            const float2 m = toViewport(e.button.x, e.button.y);
            if (marquee_) {
                marquee_ = false;
                boxSelect(clickAt_, m, clickMode_);
            } else if (inside(m) && len2(m - clickAt_) < 4.0f) {
                uint32_t target;
                float w[3][4];
                if (elementTarget(target, w)) {
                    elementClick(m, clickMode_);  // (elements: picked on the CPU, at once)
                } else {
                    pickMode_ = clickMode_;
                    renderer_->requestPick(uint32_t(m.x), uint32_t(m.y));
                }
            }
            return true;
        }
        return false;
    case SDL_EVENT_KEY_DOWN: {
        if (e.key.key == SDLK_ESCAPE && drag_.active) {
            endDrag(true);
            return true;
        }
        if (tools_.tool == SceneTool::Measure && (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_RETURN)) {  // done measuring
            tools_.tool = SceneTool::Move;
            measured_ = measuring_ = false;
            bridge_->viewportHint("");
            bridge_->toolsChanged();
            return true;
        }
        // The view: Numpad 5 orthographic or not; 1 front, 3 right, 7 top (Ctrl: back, left, bottom).
        if (!e.key.repeat && (e.key.key == SDLK_KP_5 || e.key.key == SDLK_KP_1 || e.key.key == SDLK_KP_3 || e.key.key == SDLK_KP_7)) {
            const float s = (e.key.mod & SDL_KMOD_CTRL) ? -1.0f : 1.0f;
            if (e.key.key == SDLK_KP_5)
                camera_->setOrthographic(!camera_->orthographic());
            else
                camera_->lookAlong(e.key.key == SDLK_KP_1 ? float3{0, 0, -s} : e.key.key == SDLK_KP_3 ? float3{-s, 0, 0} : float3{0, -s, 0});
            tools_.orthographic = camera_->orthographic();
            bridge_->toolsChanged();
            return true;
        }
        if (tools_.tool == SceneTool::Spline && !e.key.repeat) {
            // Esc: a drag put back, else the tool left (Enter too); C: closed or open; V: the point
            // under the mouse a corner or smooth; X: it taken out.
            if (e.key.key == SDLK_ESCAPE && splining_) {
                splining_ = false;
                bridge_->splineDragEnd(true);
                return true;
            }
            if (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) {
                tools_.tool = SceneTool::Move;
                bridge_->toolsChanged();
                return true;
            }
            if (e.key.key == SDLK_C && !(e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) {
                bridge_->splineToggle(-1);
                return true;
            }
            if (e.key.key == SDLK_V && splineHover_ >= 0 && !(e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) {
                bridge_->splineToggle(splineHover_);
                return true;
            }
            if (e.key.key == SDLK_X && splineHover_ >= 0 && !(e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) {
                bridge_->splineRemove(uint32_t(splineHover_));
                return true;
            }
        }
        if (e.key.key == SDLK_ESCAPE && tools_.tool == SceneTool::Paint && !painting_) {  // stop painting
            tools_.tool = SceneTool::Move;
            bridge_->toolsChanged();
            return true;
        }
        if (e.key.key == SDLK_ESCAPE && tools_.tool == SceneTool::Brush && !brushing_) {  // stop brushing
            tools_.tool = SceneTool::Move;
            bridge_->toolsChanged();
            return true;
        }
        if (camera_->flying() || (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)))
            return false;
        // Editor code's commands bound to a key (in an element mode, or any; Alt ones too: Alt+N),
        // then Shift+R: again.
        if (!e.key.repeat && bridge_->commandKey(e.key.key, e.key.mod, uint32_t(tools_.element)))
            return true;
        if (e.key.mod & SDL_KMOD_ALT)
            return false;
        if (brushTool && (e.key.key == SDLK_LEFTBRACKET || e.key.key == SDLK_RIGHTBRACKET)) {  // (repeats too)
            terrain->scaleRadius(e.key.key == SDLK_RIGHTBRACKET ? 1.15f : 1.0f / 1.15f);
            return true;
        }
        if (tools_.tool == SceneTool::Paint && (e.key.key == SDLK_LEFTBRACKET || e.key.key == SDLK_RIGHTBRACKET)) {
            tools_.paintRadius = std::clamp(tools_.paintRadius * (e.key.key == SDLK_RIGHTBRACKET ? 1.15f : 1.0f / 1.15f), 0.01f, 50.0f);
            bridge_->toolsChanged();
            return true;
        }
        if (e.key.repeat)
            return false;
        if (e.key.key == SDLK_R && (e.key.mod & SDL_KMOD_SHIFT))
            return bridge_->repeatCommand();
        // Shift+digits are the page's chord menus (Shift+1..5 open one, the digits pick while Shift is
        // held): not the element modes' keys.
        if ((e.key.mod & SDL_KMOD_SHIFT) && e.key.key >= SDLK_0 && e.key.key <= SDLK_9)
            return false;
        switch (e.key.key) {
        case SDLK_Q: tools_.tool = SceneTool::View; break;
        case SDLK_W: tools_.tool = SceneTool::Move; break;
        case SDLK_E: tools_.tool = SceneTool::Rotate; break;
        case SDLK_R: tools_.tool = SceneTool::Scale; break;
        case SDLK_B: tools_.tool = SceneTool::Brush; break;
        case SDLK_X: tools_.local = !tools_.local; break;
        case SDLK_Z: tools_.pivotCenter = !tools_.pivotCenter; break;
        case SDLK_F: frameSelection(); return true;
        case SDLK_1: tools_.element = ElementMode::Object; break;
        case SDLK_2: tools_.element = ElementMode::Vertex; break;
        case SDLK_3: tools_.element = ElementMode::Edge; break;
        case SDLK_4: tools_.element = ElementMode::Face; break;
        default: return false;
        }
        if (drag_.active)
            endDrag(false);
        bridge_->toolsChanged();
        return true;
    }
    default:
        return false;
    }
}

// ---- per frame

void SceneView::update(const UiRect& viewport, const CameraState& camera)
{
    FX_ZONE_CAT("SceneView::update", Editor);
    if (!device_)
        return;
    viewport_ = viewport;
    camPos_ = camera.position;
    camRight_ = camera.right;
    camUp_ = camera.up;
    camForward_ = camera.forward;
    p00_ = camera.p00;
    p11_ = camera.p11;
    zNear_ = camera.zNear;

    uint32_t picked, pickedCluster, pickedTriangle;
    if (renderer_->pickResult(picked, &pickedCluster, &pickedTriangle)) {
        bridge_->notePick(picked, pickedCluster, pickedTriangle);
        if (picked != ~0u || tools_.tool != SceneTool::Paint)  // (painting: a click on nothing keeps the mesh)
            bridge_->selectSlot(picked, EditorBridge::SelectMode(pickMode_));
        pickMode_ = 0;
    }
    if (drag_.active && bridge_->selected() != drag_.entity)
        endDrag(true);  // the selection changed under the drag (undo, delete)

    // The brush decal follows the mouse over the terrain.
    const TerrainEditor* terrain = bridge_->terrainEditor();
    brushHover_ = tools_.tool == SceneTool::Brush && terrain && terrain->valid() && !tools_.gameView &&
                  (brushing_ || (mouseInside_ && !camera_->busy())) && brushHit(mouse_, brushAt_);
    if (tools_.tool == SceneTool::Paint && !tools_.gameView && (painting_ || (mouseInside_ && !camera_->busy()))) {
        float3 normal;
        brushHover_ = bridge_->paintHover(camPos_, rayDirection(mouse_), brushAt_, normal);
    }

    splineHover_ = tools_.tool == SceneTool::Spline && mouseInside_ && !camera_->busy() && !tools_.gameView ? splinePointAt(mouse_) : -1;
    markers_.clear();
    bridge_->markers(markers_);
    const bool idle = !drag_.active && !camera_->busy() && mouseInside_ && !tools_.gameView;
    hover_ = idle ? hitGizmo(mouse_) : (drag_.active ? drag_.handle : Handle::None);
    hoverAxis_ = idle ? hitOrientation(mouse_) : -1;
    buildGeometry();
}

void SceneView::line(float2 a, float2 b, float width, uint32_t color)
{
    const float2 d = b - a;
    const float l = len2(d);
    if (l < 1e-4f)
        return;
    const float2 n = float2{-d.y / l, d.x / l} * (width * 0.5f);
    quad(a + n, b + n, b - n, a - n, color);
}

void SceneView::quad(float2 a, float2 b, float2 c, float2 d, uint32_t color)
{
    if (vertices_.size() + 6 > kMaxVertices)
        return;
    const float sx = 2.0f / float(viewport_.w), sy = 2.0f / float(viewport_.h);
    auto v = [&](float2 p) { vertices_.push_back({p.x * sx - 1.0f, 1.0f - p.y * sy, 0.0f, color}); };
    v(a), v(b), v(c), v(a), v(c), v(d);
}

void SceneView::disc(float2 center, float radius, uint32_t color, int segments)
{
    for (int k = 0; k < segments; k += 2) {
        auto at = [&](int i) {
            const float t = float(i) / float(segments) * 2.0f * kPi;
            return center + float2{std::cos(t), std::sin(t)} * radius;
        };
        quad(center, at(k), at(k + 1), at(k + 2), color);
    }
}

void SceneView::buildWorldLines()
{
    for (const WorldLine& l : worldLines_) {
        float2 a, b;
        if (project(l.a, a) && project(l.b, b))
            line(a, b, 1.5f, l.color);
    }
}

void SceneView::buildGeometry()
{
    vertices_.clear();
    if (viewport_.w == 0)
        return;
    if (tools_.gameView) {  // (the Game tab: only scripts' debug lines)
        buildWorldLines();
        return;
    }
    buildWorldLines();
    buildElements();  // (a level mesh's edges, vertices and chosen elements in an element mode)
    if (marquee_ && clickPending_) {  // the selection box
        const float2 a = clickAt_, b = mouse_;
        quad(a, {b.x, a.y}, b, {a.x, b.y}, rgba(0.38f, 0.69f, 1.0f, 0.18f));
        const uint32_t edge = rgba(0.38f, 0.69f, 1.0f, 0.8f);
        line(a, {b.x, a.y}, 1.0f, edge);
        line({b.x, a.y}, b, 1.0f, edge);
        line(b, {a.x, b.y}, 1.0f, edge);
        line({a.x, b.y}, a, 1.0f, edge);
    }
    const Handle active = hover_;
    const Handle axisHandle[3] = {Handle::X, Handle::Y, Handle::Z};
    auto axisColor = [&](int a) { return active == axisHandle[a] ? kActive : kAxisColor[a]; };

    // Transform gizmo.
    const Gizmo g = drag_.active ? [&] {
        Gizmo live = drag_.gizmo0;  // axes stay put during the drag; the handle follows the entity
        float w[3][4];
        if (float3 c; drag_.elements && tools_.tool == SceneTool::Move && elementCenter(c))
            live.origin = c;  // (or the elements)
        else if (tools_.tool == SceneTool::Move && bridge_->worldOf(drag_.entity, w))
            live.origin = live.origin + float3{w[0][3], w[1][3], w[2][3]} -
                          float3{drag_.world0[0][3], drag_.world0[1][3], drag_.world0[2][3]};
        return live;
    }()
                                 : buildGizmo();
    float2 o;
    if (g.valid && project(g.origin, o)) {
        const float3 view = normalize(g.origin - camPos_);
        if (tools_.tool == SceneTool::Rotate) {
            // View ring, then each axis ring: front half bright, back half faint.
            for (int k = 0; k < 64; ++k) {
                const float t0 = float(k) / 64.0f * 2.0f * kPi, t1 = float(k + 1) / 64.0f * 2.0f * kPi;
                line(o + float2{std::cos(t0), std::sin(t0)} * (kGizmoPixels * 1.15f),
                     o + float2{std::cos(t1), std::sin(t1)} * (kGizmoPixels * 1.15f), active == Handle::ViewRing ? 3.0f : 1.6f,
                     active == Handle::ViewRing ? kActive : kViewRing);
            }
            for (int a = 0; a < 3; ++a) {
                const float3 u = normalize(cross(g.axis[a], std::fabs(g.axis[a].y) < 0.9f ? float3{0, 1, 0} : float3{1, 0, 0}));
                const float3 v = cross(g.axis[a], u);
                for (int k = 0; k < 64; ++k) {
                    const float t0 = float(k) / 64.0f * 2.0f * kPi, t1 = float(k + 1) / 64.0f * 2.0f * kPi;
                    const float3 p0 = g.origin + (u * std::cos(t0) + v * std::sin(t0)) * g.size;
                    const float3 p1 = g.origin + (u * std::cos(t1) + v * std::sin(t1)) * g.size;
                    float2 s0, s1;
                    if (!project(p0, s0) || !project(p1, s1))
                        continue;
                    const bool front = dot(p0 + p1 - g.origin * 2.0f, view) <= 0.0f;
                    line(s0, s1, front ? (active == axisHandle[a] ? 3.2f : 2.2f) : 1.2f,
                         front ? axisColor(a) : withAlpha(kAxisColor[a], 0.25f));
                }
            }
            disc(o, 3.0f, kCenter);
        } else {
            for (int a = 0; a < 3; ++a) {
                const float facing = std::fabs(dot(g.axis[a], view));
                if (facing > 0.97f)
                    continue;
                float2 e;
                if (!project(g.origin + g.axis[a] * g.size, e))
                    continue;
                const uint32_t c = withAlpha(axisColor(a), facing > 0.9f ? (0.97f - facing) / 0.07f : 1.0f);
                line(o, e, active == axisHandle[a] ? 3.2f : 2.2f, c);
                const float2 d = e - o;
                const float l = len2(d);
                if (l < 1.0f)
                    continue;
                const float2 dir = d * (1.0f / l), n = {-dir.y, dir.x};
                if (tools_.tool == SceneTool::Move) {  // arrow head
                    const float2 tip = e + dir * 16.0f;
                    quad(e + n * 6.0f, tip, tip, e - n * 6.0f, c);
                } else {  // scale: a box at the end
                    quad(e + n * 6.0f - dir * 6.0f, e + n * 6.0f + dir * 6.0f, e - n * 6.0f + dir * 6.0f,
                         e - n * 6.0f - dir * 6.0f, c);
                }
            }
            if (tools_.tool == SceneTool::Move) {
                const Handle planes[3] = {Handle::PlaneYZ, Handle::PlaneXZ, Handle::PlaneXY};
                for (int n = 0; n < 3; ++n) {
                    const float3 a = g.axis[(n + 1) % 3], b = g.axis[(n + 2) % 3];
                    float2 q[4];
                    const float lo = g.size * 0.15f, hi = g.size * 0.4f;
                    if (project(g.origin + a * lo + b * lo, q[0]) && project(g.origin + a * hi + b * lo, q[1]) &&
                        project(g.origin + a * hi + b * hi, q[2]) && project(g.origin + a * lo + b * hi, q[3]))
                        quad(q[0], q[1], q[2], q[3], active == planes[n] ? withAlpha(kActive, 0.6f) : withAlpha(kAxisColor[n], 0.35f));
                }
            }
            const float r = active == Handle::Center ? 7.0f : 5.5f;
            const uint32_t cc = active == Handle::Center ? kActive : kCenter;
            quad(o + float2{-r, -r}, o + float2{r, -r}, o + float2{r, r}, o + float2{-r, r}, cc);
        }
    }

    // Icons of entities without a mesh (under the transform gizmo); the selected light shows its
    // reach: a sphere of its range, or its cone.
    const auto worldLine = [&](float3 a, float3 b, float width, uint32_t color) {
        float2 pa, pb;
        if (project(a, pa) && project(b, pb))
            line(pa, pb, width, color);
    };
    const auto worldCircle = [&](float3 center, float3 u, float3 v, float radius, uint32_t color) {
        for (int k = 0; k < 48; ++k) {
            const float t0 = float(k) / 48.0f * 2.0f * kPi, t1 = float(k + 1) / 48.0f * 2.0f * kPi;
            worldLine(center + (u * std::cos(t0) + v * std::sin(t0)) * radius, center + (u * std::cos(t1) + v * std::sin(t1)) * radius,
                      1.4f, color);
        }
    };
    // The Audio tab's probes: the region the next bake covers (a box), and the baked probes (dots,
    // the nearest few thousand in front of the view).
    if (bridge_->showAudioProbes()) {
        const uint32_t ink = rgba(0.3f, 0.85f, 0.75f, 0.9f);
        if (float3 lo, hi; bridge_->audioBakeRegion(lo, hi)) {
            const auto corner = [&](int i) { return float3{(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z}; };
            for (int i = 0; i < 8; ++i)
                for (int axis = 1; axis < 8; axis <<= 1)
                    if (!(i & axis))
                        worldLine(corner(i), corner(i | axis), 1.4f, withAlpha(ink, 0.7f));
        }
        if (const AudioBakedData* baked = bridge_->audioBaked()) {
            std::vector<std::pair<float, float3>> near;
            for (const AudioProbe& probe : baked->probes)
                if (const float d = dot(probe.position - camPos_, camForward_); d > zNear_)
                    near.emplace_back(length(probe.position - camPos_), probe.position);
            const size_t keep = std::min<size_t>(near.size(), 2000);
            std::partial_sort(near.begin(), near.begin() + ptrdiff_t(keep), near.end(),
                              [](const auto& a, const auto& b) { return a.first < b.first; });
            for (size_t i = 0; i < keep; ++i)
                if (float2 q; project(near[i].second, q))
                    quad(q + float2{-2.5f, -2.5f}, q + float2{2.5f, -2.5f}, q + float2{2.5f, 2.5f}, q + float2{-2.5f, 2.5f}, ink);
        }
    }
    if (measured_ && tools_.tool == SceneTool::Measure) {
        // The ruler: its line, its run along x, y and z (red, green, blue), its ends.
        const float3 a = measure_[0], b = measure_[1];
        const float3 ax = {b.x, a.y, a.z}, axy = {b.x, b.y, a.z};
        worldLine(a, ax, 1.4f, rgba(1.0f, 0.35f, 0.3f, 0.8f));
        worldLine(ax, axy, 1.4f, rgba(0.4f, 1.0f, 0.4f, 0.8f));
        worldLine(axy, b, 1.4f, rgba(0.35f, 0.6f, 1.0f, 0.8f));
        worldLine(a, b, 2.4f, rgba(1.0f, 1.0f, 1.0f, 0.95f));
        for (const float3& end : {a, b})
            if (float2 q; project(end, q))
                quad(q + float2{-4, -4}, q + float2{4, -4}, q + float2{4, 4}, q + float2{-4, 4}, rgba(1.0f, 0.85f, 0.2f, 1.0f));
    }
    for (const WorldLine& l : previewLines_)  // a tool's outline of what a click would make
        worldLine(l.a, l.b, 2.0f, l.color);
    if (tools_.showNav) {  // the navigation mesh: its polygons' edges (those near enough)
        const std::vector<float3>& nav = bridge_->navLines();
        const uint32_t ink = rgba(0.25f, 0.9f, 0.55f, 0.75f);
        for (size_t k = 0; k + 1 < nav.size(); k += 2)
            if (length(nav[k] - camPos_) < 120.0f)
                worldLine(nav[k], nav[k + 1], 1.2f, ink);
    }
    for (const EntityMarker& mk : markers_) {
        float2 p;
        const float3 at = {mk.world[0][3], mk.world[1][3], mk.world[2][3]};
        if (!markerShown(mk))
            continue;
        if (mk.volumeKind) {
            // A gameplay volume: its outline, coloured by kind (trigger green, kill red, ladder
            // yellow, audio blue, nav blocker purple, nav area orange), with it, its parent or a child
            // selected (outlineShown); brighter selected.
            static const float kInk[6][3] = {{0.3f, 0.95f, 0.45f}, {1.0f, 0.3f, 0.25f}, {1.0f, 0.85f, 0.2f},
                                             {0.35f, 0.65f, 1.0f}, {0.75f, 0.4f, 1.0f}, {1.0f, 0.6f, 0.2f}};
            const float* c = kInk[(mk.volumeKind - 1) % 6];
            const bool sel = mk.index == bridge_->selected();
            for (size_t k = 0; outlineShown(mk) && k + 1 < mk.volumeEdges.size(); k += 2)
                worldLine(mk.volumeEdges[k], mk.volumeEdges[k + 1], sel ? 2.2f : 1.4f, sel ? kActive : rgba(c[0], c[1], c[2], 0.85f));
        }
        if (mk.snapPoint) {
            // A snap point: its out (+z) and up (+y), a quarter metre.
            const float3 ax = normalize(float3{mk.world[0][2], mk.world[1][2], mk.world[2][2]});
            const float3 ay = normalize(float3{mk.world[0][1], mk.world[1][1], mk.world[2][1]});
            const bool sel = mk.index == bridge_->selected();
            worldLine(at, at + ax * 0.25f, sel ? 2.4f : 1.8f, rgba(0.2f, 0.85f, 1.0f, 0.95f));
            worldLine(at + ax * 0.25f, at + ax * 0.18f + ay * 0.05f, 1.6f, rgba(0.2f, 0.85f, 1.0f, 0.95f));
            worldLine(at + ax * 0.25f, at + ax * 0.18f - ay * 0.05f, 1.6f, rgba(0.2f, 0.85f, 1.0f, 0.95f));
            worldLine(at, at + ay * 0.15f, 1.4f, rgba(0.4f, 1.0f, 0.4f, 0.9f));
        }
        if (mk.cutShape) {
            // A cut volume: its unit shape by its transform (an opening: where its wall puts it), when
            // it, its wall or what is under it is selected (markerShown); brighter selected.
            const float(*cw)[4] = mk.opening ? mk.openingWorld : mk.world;
            const float3 co = {cw[0][3], cw[1][3], cw[2][3]};
            const float3 ax = {cw[0][0], cw[1][0], cw[2][0]}, ay = {cw[0][1], cw[1][1], cw[2][1]}, az = {cw[0][2], cw[1][2], cw[2][2]};
            const auto pt = [&](float x, float y, float z) { return co + ax * x + ay * y + az * z; };
            const uint32_t wire = mk.index == bridge_->selected() ? kActive : rgba(1.0f, 0.55f, 0.2f, 0.7f);
            const auto ring = [&](float y, float r, int axis) {  // a circle round y (axis 1), x (0) or z (2)
                for (int k = 0; k < 32; ++k) {
                    const float t0 = float(k) / 32.0f * 2.0f * kPi, t1 = float(k + 1) / 32.0f * 2.0f * kPi;
                    const auto on = [&](float t) {
                        const float c = r * std::cos(t), s = r * std::sin(t);
                        return axis == 1 ? pt(c, y, s) : axis == 0 ? pt(y, c, s) : pt(c, s, y);
                    };
                    worldLine(on(t0), on(t1), 1.4f, wire);
                }
            };
            if (mk.cutShape == 1 || mk.cutShape == 4) {
                const float sx[4] = {-0.5f, 0.5f, 0.5f, -0.5f}, sz[4] = {-0.5f, -0.5f, 0.5f, 0.5f};
                for (int k = 0; k < 4; ++k) {
                    const int n = (k + 1) % 4;
                    worldLine(pt(sx[k], -0.5f, sz[k]), pt(sx[n], -0.5f, sz[n]), 1.4f, wire);
                    worldLine(pt(sx[k], 0.5f, sz[k]), pt(sx[n], 0.5f, sz[n]), 1.4f, wire);
                    worldLine(pt(sx[k], -0.5f, sz[k]), pt(sx[k], 0.5f, sz[k]), 1.4f, wire);
                }
            } else if (mk.cutShape == 2) {
                ring(-0.5f, 0.5f, 1);
                ring(0.5f, 0.5f, 1);
                for (int k = 0; k < 4; ++k) {
                    const float t = float(k) * 0.5f * kPi;
                    worldLine(pt(0.5f * std::cos(t), -0.5f, 0.5f * std::sin(t)), pt(0.5f * std::cos(t), 0.5f, 0.5f * std::sin(t)), 1.4f, wire);
                }
            } else {
                ring(0.0f, 0.5f, 0);
                ring(0.0f, 0.5f, 1);
                ring(0.0f, 0.5f, 2);
            }
        }
        if (!mk.splineCurve.empty() && outlineShown(mk)) {
            // A spline: its curve, with it, its parent or a child (a fence along it) selected, or the
            // Spline tool on (outlineShown); selected, its points (the first a little bigger, the one
            // under the mouse bigger still).
            const bool sel = mk.index == bridge_->selected();
            const uint32_t ink = sel ? kActive : rgba(0.95f, 0.75f, 0.25f, 0.85f);
            for (size_t k = 0; k + 1 < mk.splineCurve.size(); ++k)
                worldLine(mk.splineCurve[k], mk.splineCurve[k + 1], sel ? 2.2f : 1.5f, ink);
            if (sel || tools_.tool == SceneTool::Spline)
                for (size_t k = 0; k < mk.splinePoints.size(); ++k)
                    if (float2 q; project(mk.splinePoints[k], q)) {
                        const float r = sel && int32_t(k) == splineHover_ ? 6.5f : k == 0 ? 5.0f : 4.0f;
                        quad(q + float2{-r, -r}, q + float2{r, -r}, q + float2{r, r}, q + float2{-r, r}, rgba(0.08f, 0.08f, 0.1f, 0.9f));
                        const float i = r - 1.5f;
                        quad(q + float2{-i, -i}, q + float2{i, -i}, q + float2{i, i}, q + float2{-i, i}, ink);
                    }
        }
        if (mk.kind == EntityKind::Water) {
            // Its outline on the surface (a tank: its box), whatever of it is in view.
            const bool sel = mk.index == bridge_->selected();
            const uint32_t edge = sel ? kActive : rgba(0.25f, 0.65f, 1.0f, 0.8f);
            float3 ax = normalize(float3{mk.world[0][0], 0.0f, mk.world[2][0]});
            if (length(ax) < 0.5f)
                ax = {1.0f, 0.0f, 0.0f};
            const float3 az = {-ax.z, 0.0f, ax.x};  // (y up: x turned a quarter toward +z)
            const auto local = [&](float x, float z, float y = 0.0f) { return at + ax * x + az * z + float3{0.0f, y, 0.0f}; };
            const float hw = mk.waterSize[0] * 0.5f, hl = mk.waterSize[1] * 0.5f;
            switch (WaterShape(mk.waterShape)) {
            case WaterShape::Circle: worldCircle(at, ax, az, hw, edge); break;
            case WaterShape::Polygon:
                for (uint32_t k = 0; k < mk.waterPointCount; ++k) {
                    const uint32_t j = (k + 1) % mk.waterPointCount;
                    worldLine(local(mk.waterPoints[k * 2], mk.waterPoints[k * 2 + 1]), local(mk.waterPoints[j * 2], mk.waterPoints[j * 2 + 1]),
                              1.6f, edge);
                }
                break;
            case WaterShape::Ocean: break;
            case WaterShape::River:  // its centre line
                for (uint32_t k = 0; k + 1 < mk.waterPointCount; ++k)
                    worldLine(local(mk.waterPoints[k * 2], mk.waterPoints[k * 2 + 1]),
                              local(mk.waterPoints[k * 2 + 2], mk.waterPoints[k * 2 + 3]), 1.6f, edge);
                break;
            default: {
                const float xs[4] = {-hw, hw, hw, -hw}, zs[4] = {-hl, -hl, hl, hl};
                for (int k = 0; k < 4; ++k) {
                    worldLine(local(xs[k], zs[k]), local(xs[(k + 1) % 4], zs[(k + 1) % 4]), 1.6f, edge);
                    if (WaterShape(mk.waterShape) == WaterShape::Box) {
                        worldLine(local(xs[k], zs[k], -mk.waterSize[2]), local(xs[(k + 1) % 4], zs[(k + 1) % 4], -mk.waterSize[2]), 1.2f, edge);
                        worldLine(local(xs[k], zs[k]), local(xs[k], zs[k], -mk.waterSize[2]), 1.2f, edge);
                    }
                }
            }
            }
            if (!project(at, p))
                continue;
            // Icon: a drop of water.
            disc(p, 9.0f, sel ? kActive : rgba(0.1f, 0.1f, 0.1f, 0.6f), 16);
            disc(p + float2{0.0f, 1.5f}, 5.0f, rgba(0.25f, 0.65f, 1.0f), 16);
            quad(p + float2{-3.6f, -0.5f}, p + float2{0.0f, -7.0f}, p + float2{3.6f, -0.5f}, p + float2{0.0f, 0.0f}, rgba(0.25f, 0.65f, 1.0f));
            continue;
        }
        if (!project(at, p))
            continue;
        const bool selected = mk.index == bridge_->selected();
        const float pixel = 2.0f * dot(at - camPos_, camForward_) / (p11_ * float(viewport_.h));  // world size of a pixel
        if (mk.audio) {
            // An Audio Source: selected and 3D, its full-volume and silent distances (a ring flat on
            // the ground for each, and the far one upright too).
            if (selected && mk.audioSpatial) {
                const uint32_t wire = withAlpha(kActive, 0.8f), far = withAlpha(kActive, 0.45f);
                worldCircle(at, {1, 0, 0}, {0, 0, 1}, mk.audioMin, wire);
                worldCircle(at, {1, 0, 0}, {0, 0, 1}, mk.audioMax, far);
                worldCircle(at, {1, 0, 0}, {0, 1, 0}, mk.audioMax, far);
                worldCircle(at, {0, 1, 0}, {0, 0, 1}, mk.audioMax, far);
            }
            // A speaker, on an entity without an icon of its own.
            if (mk.kind == EntityKind::Empty || mk.kind == EntityKind::Mesh || mk.kind == EntityKind::Primitive) {
                // Playing: from white (a clear path to the listener) to red (blocked).
                const float o = mk.audioOcclusion < 0.0f ? 1.0f : std::clamp(mk.audioOcclusion, 0.0f, 1.0f);
                const uint32_t ink = selected ? kActive : rgba(0.9f, 0.2f + 0.7f * o, 0.2f + 0.7f * o, 0.95f);
                disc(p, 11.0f, rgba(0.1f, 0.1f, 0.1f, 0.6f), 20);
                quad(p + float2{-7, -2.5f}, p + float2{-3.5f, -2.5f}, p + float2{-3.5f, 2.5f}, p + float2{-7, 2.5f}, ink);
                quad(p + float2{-3.5f, -2.5f}, p + float2{1, -6}, p + float2{1, 6}, p + float2{-3.5f, 2.5f}, ink);
                for (int k = 0; k < 2; ++k) {  // two sound waves
                    const float r = 4.0f + 3.5f * float(k);
                    for (int s = 0; s < 4; ++s) {
                        const float a0 = -0.9f + 1.8f * float(s) / 4.0f, a1 = -0.9f + 1.8f * float(s + 1) / 4.0f;
                        line(p + float2{2.0f + r * std::cos(a0), r * std::sin(a0)}, p + float2{2.0f + r * std::cos(a1), r * std::sin(a1)}, 1.5f, ink);
                    }
                }
                continue;
            }
        }
        if (mk.effect || mk.decal || mk.emitterShape) {
            // Particles: selected, the emitter's shape (+Y its axis) or the decal's box (projecting
            // along -Y); the icon: a sparkle (an effect) or a stamp (a decal).
            const float3 ax = {mk.world[0][0], mk.world[1][0], mk.world[2][0]}, ay = {mk.world[0][1], mk.world[1][1], mk.world[2][1]},
                         az = {mk.world[0][2], mk.world[1][2], mk.world[2][2]};
            const auto pt = [&](float x, float y, float z) { return at + ax * x + ay * y + az * z; };
            const uint32_t wire = withAlpha(kActive, 0.75f);
            if (selected && mk.emitterShape == 1) {
                const float r = mk.shape[0];
                worldCircle(at, normalize(ax), normalize(az), r * length(ax), wire);
                worldCircle(at, normalize(ax), normalize(ay), r * length(ax), wire);
                worldCircle(at, normalize(ay), normalize(az), r * length(ay), wire);
            } else if (selected && mk.emitterShape == 2) {
                const float hx = mk.shape[0] * 0.5f, hy = mk.shape[1] * 0.5f, hz = mk.shape[2] * 0.5f;
                const float sx[4] = {-hx, hx, hx, -hx}, sz[4] = {-hz, -hz, hz, hz};
                for (int k = 0; k < 4; ++k) {
                    const int n = (k + 1) % 4;
                    worldLine(pt(sx[k], -hy, sz[k]), pt(sx[n], -hy, sz[n]), 1.4f, wire);
                    worldLine(pt(sx[k], hy, sz[k]), pt(sx[n], hy, sz[n]), 1.4f, wire);
                    worldLine(pt(sx[k], -hy, sz[k]), pt(sx[k], hy, sz[k]), 1.4f, wire);
                }
            } else if (selected && mk.emitterShape == 3) {
                const float angle = std::clamp(mk.shape[0], 0.0f, 89.0f) * (kPi / 180.0f), r0 = mk.shape[1], len = mk.shape[2];
                const float r1 = r0 + std::tan(angle) * len;
                for (int k = 0; k < 48; ++k) {
                    const float t0 = float(k) / 48.0f * 2.0f * kPi, t1 = float(k + 1) / 48.0f * 2.0f * kPi;
                    worldLine(pt(r0 * std::cos(t0), 0, r0 * std::sin(t0)), pt(r0 * std::cos(t1), 0, r0 * std::sin(t1)), 1.4f, wire);
                    worldLine(pt(r1 * std::cos(t0), len, r1 * std::sin(t0)), pt(r1 * std::cos(t1), len, r1 * std::sin(t1)), 1.4f, wire);
                }
                for (int k = 0; k < 4; ++k) {
                    const float t = float(k) / 4.0f * 2.0f * kPi;
                    worldLine(pt(r0 * std::cos(t), 0, r0 * std::sin(t)), pt(r1 * std::cos(t), len, r1 * std::sin(t)), 1.4f, wire);
                }
            } else if (selected && mk.emitterShape == 4) {
                const float r = mk.shape[0], arc = std::clamp(mk.shape[1], 0.0f, 360.0f) * (kPi / 180.0f);
                for (int k = 0; k < 48; ++k) {
                    const float t0 = arc * float(k) / 48.0f, t1 = arc * float(k + 1) / 48.0f;
                    worldLine(pt(r * std::cos(t0), 0, r * std::sin(t0)), pt(r * std::cos(t1), 0, r * std::sin(t1)), 1.4f, wire);
                }
            }
            if (selected && mk.decal) {
                const float hx = mk.decalSize[0] * 0.5f, hz = mk.decalSize[2] * 0.5f, d = mk.decalSize[1];
                const float sx[4] = {-hx, hx, hx, -hx}, sz[4] = {-hz, -hz, hz, hz};
                for (int k = 0; k < 4; ++k) {
                    const int n = (k + 1) % 4;
                    worldLine(pt(sx[k], 0, sz[k]), pt(sx[n], 0, sz[n]), 1.6f, wire);
                    worldLine(pt(sx[k], -d, sz[k]), pt(sx[n], -d, sz[n]), 1.2f, withAlpha(kActive, 0.4f));
                    worldLine(pt(sx[k], 0, sz[k]), pt(sx[k], -d, sz[k]), 1.2f, withAlpha(kActive, 0.4f));
                }
                worldLine(at, pt(0, -d, 0), 1.6f, wire);  // the way it projects
            }
            if (mk.kind == EntityKind::Empty || mk.kind == EntityKind::Mesh || mk.kind == EntityKind::Primitive) {
                const uint32_t ink = selected ? kActive : mk.effect ? rgba(1.0f, 0.78f, 0.3f, 0.95f) : rgba(0.85f, 0.85f, 0.9f, 0.95f);
                disc(p, 11.0f, rgba(0.1f, 0.1f, 0.1f, 0.6f), 20);
                if (mk.effect || mk.emitterShape) {  // a four-pointed star, and a small one beside it
                    quad(p + float2{0, -8}, p + float2{2, -2}, p + float2{0, 0}, p + float2{-2, -2}, ink);
                    quad(p + float2{8, 0}, p + float2{2, 2}, p + float2{0, 0}, p + float2{2, -2}, ink);
                    quad(p + float2{0, 8}, p + float2{-2, 2}, p + float2{0, 0}, p + float2{2, 2}, ink);
                    quad(p + float2{-8, 0}, p + float2{-2, -2}, p + float2{0, 0}, p + float2{-2, 2}, ink);
                    disc(p + float2{6, -6}, 1.6f, ink, 8);
                } else {  // a stamp: a disc on a plate
                    disc(p + float2{0, -2.5f}, 3.5f, ink, 12);
                    quad(p + float2{-7, 4}, p + float2{7, 4}, p + float2{4, 7}, p + float2{-4, 7}, ink);
                }
                continue;
            }
        }
        if (mk.kind == EntityKind::Light) {
            const float3 forward = normalize(float3{-mk.world[0][2], -mk.world[1][2], -mk.world[2][2]});
            const uint32_t tint = rgba(std::min(mk.color[0], 1.0f), std::min(mk.color[1], 1.0f), std::min(mk.color[2], 1.0f));
            if (mk.lightType == 2) {  // directional: a sun, with parallel rays along its direction
                const float3 u = normalize(cross(forward, std::fabs(forward.y) < 0.99f ? float3{0, 1, 0} : float3{1, 0, 0}));
                const float3 v = cross(forward, u);
                const uint32_t ray = selected ? kActive : tint;
                for (int k = 0; k < (selected ? 8 : 0); ++k) {
                    const float t = float(k) / 8.0f * 2.0f * kPi;
                    const float3 o = at + (u * std::cos(t) + v * std::sin(t)) * (14.0f * pixel);
                    worldLine(o, o + forward * (60.0f * pixel), 1.6f, withAlpha(ray, 0.8f));
                }
                disc(p, 12.0f, selected ? kActive : rgba(0.1f, 0.1f, 0.1f, 0.6f), 20);
                disc(p, 6.0f, tint, 16);
                for (int k = 0; k < 8; ++k) {
                    const float t = float(k) / 8.0f * 2.0f * kPi;
                    const float2 d = {std::cos(t), std::sin(t)};
                    line(p + d * 8.0f, p + d * 11.0f, 1.6f, rgba(0.1f, 0.1f, 0.1f));
                }
                worldLine(at, at + forward * (36.0f * pixel), 2.2f, selected ? kActive : tint);
                continue;
            }
            if (selected) {
                const uint32_t wire = withAlpha(kActive, 0.8f);
                if (mk.lightType == 1) {  // spot: four edges and the far rim of the cone
                    const float3 u = normalize(cross(forward, std::fabs(forward.y) < 0.99f ? float3{0, 1, 0} : float3{1, 0, 0}));
                    const float3 v = cross(forward, u);
                    const float rim = mk.range * std::tan(mk.outerAngle / 57.29578f);
                    const float3 end = at + forward * mk.range;
                    for (int k = 0; k < 4; ++k) {
                        const float t = float(k) * 0.5f * kPi;
                        worldLine(at, end + (u * std::cos(t) + v * std::sin(t)) * rim, 1.4f, wire);
                    }
                    worldCircle(end, u, v, rim, wire);
                } else {  // point: its range sphere, three great circles
                    worldCircle(at, {1, 0, 0}, {0, 1, 0}, mk.range, wire);
                    worldCircle(at, {1, 0, 0}, {0, 0, 1}, mk.range, wire);
                    worldCircle(at, {0, 1, 0}, {0, 0, 1}, mk.range, wire);
                }
            }
            // A bulb in the light's colour, with rays; a spot also shows where it points.
            disc(p, 10.0f, selected ? kActive : rgba(0.1f, 0.1f, 0.1f, 0.6f), 20);
            disc(p, 7.0f, tint, 16);
            for (int k = 0; k < 8; ++k) {
                const float t = float(k) / 8.0f * 2.0f * kPi;
                const float2 d = {std::cos(t), std::sin(t)};
                line(p + d * 12.0f, p + d * 16.0f, 1.6f, selected ? kActive : tint);
            }
            if (mk.lightType == 1)
                worldLine(at, at + forward * (28.0f * pixel), 2.0f, selected ? kActive : tint);
            continue;
        }
        const uint32_t ink = selected ? kActive : rgba(0.9f, 0.9f, 0.9f, 0.9f);
        if (mk.kind == EntityKind::Camera) {
            const float3 back = normalize(float3{mk.world[0][2], mk.world[1][2], mk.world[2][2]});
            const float3 up = normalize(float3{mk.world[0][1], mk.world[1][1], mk.world[2][1]});
            const float3 forward = back * -1.0f, right = normalize(cross(forward, up)), top = cross(right, forward);
            if (selected) {  // its view frustum, 2 units deep, at the scene view's aspect
                const float depth = 2.0f, h = depth * std::tan(mk.fov * 0.5f / 57.29578f);
                const float w = h * float(viewport_.w) / float(std::max(viewport_.h, 1u));
                const float3 c = at + forward * depth;
                const float3 corners[4] = {c + right * w + top * h, c - right * w + top * h, c - right * w - top * h,
                                           c + right * w - top * h};
                const uint32_t wire = withAlpha(kActive, 0.8f);
                for (int k = 0; k < 4; ++k) {
                    worldLine(at, corners[k], 1.4f, wire);
                    worldLine(corners[k], corners[(k + 1) % 4], 1.4f, wire);
                }
                worldLine(c + top * h, c + top * (h * 1.3f), 1.4f, wire);  // which way is up
            }
            // A camera body and lens, white for the main camera.
            const uint32_t body = selected ? kActive : mk.mainCamera ? rgba(0.95f, 0.95f, 0.95f) : rgba(0.65f, 0.65f, 0.65f);
            disc(p, 11.0f, rgba(0.1f, 0.1f, 0.1f, 0.6f), 20);
            quad(p + float2{-8, -5}, p + float2{3, -5}, p + float2{3, 5}, p + float2{-8, 5}, body);
            quad(p + float2{3, -2}, p + float2{8, -5}, p + float2{8, 5}, p + float2{3, 2}, body);
            worldLine(at, at + forward * (24.0f * pixel), 2.0f, body);
            continue;
        }
        disc(p, 9.0f, rgba(0.1f, 0.1f, 0.1f, 0.55f), 16);
        // Empty: its three axes, a short way along each.
        for (int a = 0; a < 3; ++a) {
            const float3 axis = normalize(float3{mk.world[0][a], mk.world[1][a], mk.world[2][a]});
            float2 e;
            if (project(at + axis * (8.0f * pixel), e))
                line(p, e, 2.0f, selected ? kActive : kAxisColor[a]);
        }
        disc(p, 2.5f, ink);
    }

    // Orientation gizmo, back to front.
    const float2 c = {float(viewport_.w) - kOrientMargin, kOrientMargin};
    int order[6] = {0, 1, 2, 3, 4, 5};
    auto dirOf = [](int k) {
        const float s = k < 3 ? 1.0f : -1.0f;
        return float3{k % 3 == 0 ? s : 0.0f, k % 3 == 1 ? s : 0.0f, k % 3 == 2 ? s : 0.0f};
    };
    std::sort(order, order + 6, [&](int a, int b) { return dot(dirOf(a), camForward_) > dot(dirOf(b), camForward_); });
    disc(c, kOrientRadius + 16.0f, rgba(0.1f, 0.1f, 0.1f, 0.35f), 32);
    for (int i = 0; i < 6; ++i) {
        const int k = order[i];
        const float3 d = dirOf(k);
        const float2 p = c + float2{dot(d, camRight_), -dot(d, camUp_)} * kOrientRadius;
        const bool hovered = k == hoverAxis_;
        if (k < 3) {
            line(c, p, 2.5f, kAxisColor[k]);
            disc(p, hovered ? 11.0f : 9.5f, hovered ? kActive : kAxisColor[k]);
            const Stroke* letter = k == 0 ? kLetterX : k == 1 ? kLetterY : kLetterZ;
            const int strokes = k == 1 ? 3 : k == 0 ? 2 : 3;
            for (int s = 0; s < strokes; ++s)
                line(p + float2{(letter[s].x0 - 0.5f) * 8.0f, (0.5f - letter[s].y0) * 8.0f},
                     p + float2{(letter[s].x1 - 0.5f) * 8.0f, (0.5f - letter[s].y1) * 8.0f}, 1.6f, rgba(0.08f, 0.08f, 0.08f));
        } else {
            disc(p, hovered ? 8.0f : 6.5f, hovered ? kActive : rgba(0.75f, 0.75f, 0.75f, 0.9f));
        }
    }
    disc(c, 5.0f, rgba(0.85f, 0.85f, 0.85f));
}

void SceneView::draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target)
{
    if (!device_ || viewport_.w == 0 || (tools_.gameView && worldLines_.empty()))
        return;
    FX_ZONE_CAT("SceneView::draw", Editor);
    FX_GPU_SECTION(cmd, "Scene View overlay");
    const uint32_t count = uint32_t(std::min<size_t>(vertices_.size(), kMaxVertices));
    if (count > 0) {
        void* mapped = SDL_MapGPUTransferBuffer(device_, vertexUpload_, true);
        std::memcpy(mapped, vertices_.data(), count * sizeof(Vertex));
        SDL_UnmapGPUTransferBuffer(device_, vertexUpload_);
        FX_GPU_PASS(cmd, "gizmo vertex upload");
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        const SDL_GPUTransferBufferLocation src = {vertexUpload_, 0};
        const SDL_GPUBufferRegion dst = {vertexBuffer_, 0, uint32_t(count * sizeof(Vertex))};
        SDL_UploadToGPUBuffer(copy, &src, &dst, true);
        SDL_EndGPUCopyPass(copy);
    }

    SDL_GPUColorTargetInfo color = {};
    color.texture = target;
    color.load_op = SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_STORE;
    FX_GPU_PASS(cmd, "grid, outline, gizmos");
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
    if (!pass)
        return;
    const SDL_GPUViewport vp = {float(viewport_.x), float(viewport_.y), float(viewport_.w), float(viewport_.h), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &vp);
    const SDL_Rect scissor = {int(viewport_.x), int(viewport_.y), int(viewport_.w), int(viewport_.h)};
    SDL_SetGPUScissor(pass, &scissor);

    const uint32_t selected = bridge_->selectedSlot();  // the outline needs a mesh
    if (tools_.grid || selected != ~0u || brushHover_) {
        struct {
            float position[4], right[4], up[4], forward[4], outline[4];
            uint32_t view[4], select[4];
            float brush[4], curve[3][4];
        } u = {};
        if (brushHover_ && tools_.tool == SceneTool::Paint) {  // the level paint brush: its radius, its falloff
            u.brush[0] = brushAt_.x;
            u.brush[1] = brushAt_.y;
            u.brush[2] = brushAt_.z;
            u.brush[3] = tools_.paintRadius;
            for (uint32_t i = 0; i < BrushSettings::kCurvePoints; ++i) {
                const float x = float(i) / float(BrushSettings::kCurvePoints - 1), t = std::clamp((x - 0.5f) * 2.0f, 0.0f, 1.0f);
                u.curve[i / 4][i % 4] = 1.0f - t * t * (3.0f - 2.0f * t);
            }
            u.curve[2][1] = tools_.paintMode == 3 ? 1.0f : 0.0f;  // (erasing: drawn as lowering)
            u.curve[2][2] = 1.0f;  // (a ball round the point under the mouse, not a column across the ground)
        } else if (const TerrainEditor* terrain = bridge_->terrainEditor(); brushHover_ && terrain) {
            const BrushSettings& b = terrain->brush();
            u.brush[0] = brushAt_.x;
            u.brush[1] = brushAt_.y;
            u.brush[2] = brushAt_.z;
            u.brush[3] = b.radius;
            for (uint32_t i = 0; i < BrushSettings::kCurvePoints; ++i)
                u.curve[i / 4][i % 4] = b.falloff[i];
            const bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
            u.curve[2][1] = b.op == BrushOp::Lower || (shift && b.op == BrushOp::Raise) ? 1.0f : 0.0f;
        }
        const float3 v[4] = {camPos_, camRight_, camUp_, camForward_};
        float* dst[4] = {u.position, u.right, u.up, u.forward};
        for (int k = 0; k < 4; ++k)
            std::memcpy(dst[k], &v[k], sizeof(float3));
        u.position[3] = zNear_;
        u.right[3] = 1.0f / p00_;
        u.up[3] = 1.0f / p11_;
        u.forward[3] = tools_.grid ? 1.0f : 0.0f;
        const float outline[4] = {0.97f, 0.56f, 0.12f, selected != ~0u ? 1.0f : 0.0f};  // Unity's selection orange
        std::memcpy(u.outline, outline, sizeof(outline));
        u.view[0] = viewport_.x;
        u.view[1] = viewport_.y;
        u.view[2] = viewport_.w;
        u.view[3] = viewport_.h;
        u.select[0] = selected;
        u.select[1] = renderer_->renderWidth();
        u.select[2] = renderer_->renderHeight();
        SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
        SDL_BindGPUGraphicsPipeline(pass, overlay_);
        const SDL_GPUTextureSamplerBinding depth = {renderer_->depthTexture(), renderer_->pointSampler()};
        SDL_BindGPUFragmentSamplers(pass, 0, &depth, 1);
        SDL_GPUTexture* vis = renderer_->visibilityTexture();
        SDL_BindGPUFragmentStorageTextures(pass, 0, &vis, 1);
        SDL_GPUBuffer* visible = renderer_->visibleClusters();
        SDL_BindGPUFragmentStorageBuffers(pass, 0, &visible, 1);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    }
    if (count > 0) {
        SDL_BindGPUGraphicsPipeline(pass, color_);
        SDL_BindGPUVertexStorageBuffers(pass, 0, &vertexBuffer_, 1);
        SDL_DrawGPUPrimitives(pass, count, 1, 0, 0);
    }
    SDL_EndGPURenderPass(pass);
}

}  // namespace fx
