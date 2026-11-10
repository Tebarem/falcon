#pragma once

// Bodies of water in a scene, on the CPU: their shapes and placement, their materials (a .fwater
// or a built-in preset, with the body's own overrides), their waves, and the height of the water
// anywhere (physics, scripts). render/water_renderer.h draws them.
//
// Shapes (all with a horizontal surface; the body's position is the surface's centre and yaw
// turns it):
//   rect     width x length, `depth` metres deep           circle   diameter = width
//   polygon  an outline of local points                     ocean    endless, at the body's height
//   river    a centre line of local points, `width` wide    box      a tank: width x depth x length
// Waves: still, Gerstner (up to 8 waves from the wind; the same sum the GPU draws), or ocean (an
// FFT spectrum, OceanSpectrum: one for the scene, from the first body that asks for it).

#include "assets/water_asset.h"
#include "core/math.h"
#include "water/ocean_spectrum.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fx {

struct Json;
class JsonWriter;

enum class WaterShape : uint32_t { Rect, Circle, Polygon, Ocean, River, Box, Count };
const char* waterShapeName(WaterShape s);
WaterShape waterShapeFromName(std::string_view name);

struct WaterBodyDesc {
    WaterShape shape = WaterShape::Rect;
    float width = 20.0f, length = 20.0f;  // metres (circle: diameter = width)
    float depth = 3.0f;                   // below the surface (boxes, and where the scene has no floor)
    std::vector<float> points;            // polygon / river: local x, z pairs
    std::vector<float> heights;           // river: each point's height over the body's (local y; missing: 0)
    std::string material = "builtin:lake";  // "builtin:<preset>" or a .fwater path (relative to the project)
    std::string overrides = "{}";          // JSON object: WaterMaterial fields that differ
    bool buoyancy = true;
};

// The water at a point: which body (-1: none), its surface height, normal and flow there, and the
// depth below the surface of the point asked about (negative: above it).
struct WaterSample {
    int32_t body = -1;
    float height = 0.0f;
    float3 normal = {0.0f, 1.0f, 0.0f};
    float3 flow = {0.0f, 0.0f, 0.0f};
    float depth = 0.0f;
};

struct GerstnerWave {
    float dirX = 1.0f, dirZ = 0.0f, amplitude = 0.0f, k = 1.0f, omega = 1.0f, steepness = 0.0f, phase = 0.0f;
};

class WaterSystem {
public:
    static constexpr uint32_t kMaxBodies = 256;
    static constexpr uint32_t kMaxWaves = 8;
    // Wave frequencies are multiples of 2 pi / kTimeWrap, so the waves repeat exactly after it and
    // the clock can wrap there (float time keeps its precision).
    static constexpr double kTimeWrap = 1200.0;

    struct Body {
        bool alive = false, visible = true;
        bool hidden = false;     // the editor's eye (on it or a parent): not drawn while the entity lives
        uint64_t entity = 0;     // the scene entity it belongs to (0: none)
        WaterBodyDesc desc;
        float3 position = {0.0f, 0.0f, 0.0f};
        float yaw = 0.0f;        // radians
        WaterMaterial material;  // resolved: its material with the overrides
        GerstnerWave waves[kMaxWaves];
        uint32_t waveCount = 0;
        float waveAmplitude = 0.0f;  // the largest vertical displacement (bounds)
        // A river's centre line, smoothed (Catmull-Rom through its points, 8 steps per span):
        // local x, z, height and distance along it, per point.
        struct PathPoint {
            float x, z, y, s;
        };
        std::vector<PathPoint> path;
    };

    // .fwater paths resolve against the project's folder.
    void setProject(std::string dir) { projectDir_ = std::move(dir); }
    void clear();

    uint32_t add(const WaterBodyDesc& desc, uint64_t entity = 0);  // its id, or ~0u (full)
    void set(uint32_t id, const WaterBodyDesc& desc);
    void remove(uint32_t id);
    void setPose(uint32_t id, float3 position, float yaw);
    void setVisible(uint32_t id, bool visible);
    void setHidden(uint32_t id, bool hidden);
    const Body* body(uint32_t id) const { return id < bodies_.size() && bodies_[id].alive ? &bodies_[id] : nullptr; }
    const std::vector<Body>& bodies() const { return bodies_; }
    uint32_t count() const;
    int32_t find(uint64_t entity) const;

    // A material file changed (the editor's water editor, a file on disk): every body using it
    // takes the new values. Returns whether any did.
    bool reloadMaterial(const std::string& path);
    // The material a body's desc names, before its overrides.
    WaterMaterial baseMaterial(const WaterBodyDesc& desc) const;

    // The water clock (seconds, wrapped at kTimeWrap): the render's waves.
    void setTime(double seconds);
    void advance(double dt) { setTime(time_ + dt); }
    float time() const { return float(time_); }
    // The simulation's clock (the physics tick's time): what sample() and the bodies' buoyancy
    // read, so floating things don't depend on frame times.
    void setSimTime(double seconds);
    float simTime() const { return float(simTime_); }

    // The water at world point p (its x, z; depth is measured from p.y), at the simulation's time.
    WaterSample sample(float3 p) const;
    bool inside(const Body& b, float x, float z) const;
    // The point of a body's still surface nearest world point p (inside its outline: right below
    // or above p; an ocean: always).
    float3 nearestSurface(const Body& b, float3 p) const;

    // Bumped whenever something the GPU holds changes (shape, pose, material, waves).
    uint64_t revision() const { return revision_; }

    // Ripples: a disturbance of the surface at world (x, z), `radius` metres, `strength` metres of
    // height (physics, the camera, scripts). The renderer takes them each frame.
    struct Splash {
        float x, z, radius, strength;
    };
    void splash(float x, float z, float radius, float strength) const;
    std::vector<Splash> takeSplashes() const;

    // The ocean spectrum (FFT waves) and the body whose material made it (-1: none). Waves per
    // cascade side: FrameSettings::water.fftSize.
    const OceanSpectrum& ocean() const { return ocean_; }
    int32_t oceanBody() const { return oceanBody_; }
    void setFftSize(uint32_t n);
    bool usesOcean(const Body& b) const { return b.material.waves == WaveMode::Ocean; }

private:
    void resolve(Body& b);
    void buildPath(Body& b);
    // The nearest point of a river's path to local (x, z): its segment, how far along it (0..1),
    // the squared distance.
    bool nearestOnPath(const Body& b, float x, float z, uint32_t& segment, float& t, float& d2) const;
    void buildWaves(Body& b);
    void updateOcean();  // the spectrum for the first body with ocean waves
    std::string resolvePath(const std::string& path) const;

    std::vector<Body> bodies_;
    std::string projectDir_;
    mutable std::unordered_map<std::string, WaterMaterial> files_;  // .fwater cache, by resolved path
    double time_ = 0.0, simTime_ = 0.0;
    uint64_t revision_ = 1;
    OceanSpectrum ocean_;
    mutable std::vector<Splash> splashes_;  // (const: physics and scripts hold the system read-only)
    int32_t oceanBody_ = -1;
    uint32_t fftSize_ = 256;
    std::string oceanKey_;  // what the spectrum was built from (rebuilt only when it changes)
};

// A body's desc as JSON (scene edits, the editor): {"shape", "width", "length", "depth", "points",
// "material", "overrides": {...}, "buoyancy"}.
void readWaterBodyJson(const Json& j, WaterBodyDesc& d);
void writeWaterBodyJson(JsonWriter& w, const char* key, const WaterBodyDesc& d);

// Body-local <-> world on the horizontal plane (the shaders' bodyToWorld / worldToBody).
float2 waterBodyToWorld(const WaterSystem::Body& b, float2 local);
float2 waterWorldToBody(const WaterSystem::Body& b, float x, float z);

}  // namespace fx
