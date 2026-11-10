#pragma once

// Baked acoustics (Steam Audio's probes), kept next to the scene in <scene>.audio/:
//   bake.json    the bake's settings (spacing, height, quality, pathing, region) and, once baked,
//                what it made and from what geometry (hashes: the stale check)
//   probes.bin   the probe batch (Steam Audio's serialized form): the listener's reverb at each
//                probe (convolution and parametric), and the paths between probes
//   probes.pos   the probes (a position and a radius each): where the baked data reaches
//
// A bake places probes on a uniform grid a fixed height above the floor under them (Steam Audio's
// "uniform floor": over the colliders and the terrain, within the region), then simulates the
// reverb at each and the paths sound takes between them. It runs on a thread of its own with its
// own copy of the geometry (the static meshes, as the audio scene places them, and the terrain's
// coarse map), so the editor goes on meanwhile.

#include "assets/acoustic_material.h"
#include "core/math.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fx {

struct AudioBakeSettings {
    float spacing = 3.0f;   // between probes, m
    float height = 1.5f;    // above the floor, m
    uint32_t quality = 2;   // 1 low, 2 medium, 3 high: rays, bounces, the tail's length
    bool pathing = true;    // paths around what is in the way (sound through doorways)
    bool autoRegion = true; // the static objects' bounds (else center, size)
    float3 center = {0.0f, 0.0f, 0.0f}, size = {64.0f, 16.0f, 64.0f};
};

// What a quality bakes with.
struct AudioBakeQuality {
    uint32_t rays, bounces;
    float seconds;  // simulated impulse response
};
inline AudioBakeQuality audioBakeQuality(uint32_t level)
{
    static const AudioBakeQuality kLevels[3] = {{8192, 16, 1.0f}, {16384, 32, 1.5f}, {32768, 64, 2.0f}};
    return kLevels[std::min(std::max(level, 1u), 3u) - 1];
}
constexpr uint32_t kAudioBakeMaxProbes = 20000;
constexpr float kAudioBakeMargin = 16.0f;  // geometry this far beyond the region is baked too (what sound bounces off)

// The scene's geometry as the bake hears it (world space; what moves takes no part).
struct AudioBakeGeometry {
    std::vector<float3> vertices;
    std::vector<uint32_t> triangles;      // three vertex indices a triangle
    std::vector<uint32_t> materialIndex;  // into materials, a triangle
    std::vector<AcousticParams> materials;
    float3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};  // the region probes are placed in
    uint64_t objectsHash = 0, terrainHash = 0;
    uint32_t objects = 0;  // static meshes
    bool terrain = false;
};

// What a bake made (bake.json "baked").
struct AudioBakeInfo {
    bool valid = false;
    uint32_t probes = 0;
    bool reverb = false, pathing = false;
    uint64_t bytes = 0;     // probes.bin
    double seconds = 0.0;   // how long it took
    int64_t bakedAt = 0;    // seconds since 1970
    uint64_t objectsHash = 0, terrainHash = 0;
    uint32_t objects = 0;
    float spacing = 0.0f, height = 0.0f, visRange = 0.0f;
    uint32_t quality = 0, order = 0;
    float3 lo = {}, hi = {};
};

struct AudioProbe {
    float3 position;
    float radius;
};

// Baked data, loaded: the probe batch (an IPLProbeBatch, released with it) and its probes.
struct AudioBakedData {
    void* batch = nullptr;
    std::vector<AudioProbe> probes;
    AudioBakeInfo info;
    AudioBakedData() = default;
    AudioBakedData(const AudioBakedData&) = delete;
    AudioBakedData& operator=(const AudioBakedData&) = delete;
    ~AudioBakedData();
    // Is `p` within a probe's reach (the baked data covers it)?
    bool covers(float3 p) const;
    void index();  // (after probes change: the lookup grid)

private:
    float cell_ = 1.0f;
    std::vector<std::pair<uint64_t, uint32_t>> grid_;  // (cell key, probe), sorted
};

std::string audioBakeDirFor(const std::string& scenePath);  // <scene without .fpak>.audio
bool readAudioBake(const std::string& dir, AudioBakeSettings& settings, AudioBakeInfo& info);  // false: none
bool writeAudioBake(const std::string& dir, const AudioBakeSettings& settings, const AudioBakeInfo& info);
// The baked files gone (the settings stay).
bool clearAudioBake(const std::string& dir, const AudioBakeSettings& settings);
// probes.bin and probes.pos into `out` (`context`: the IPLContext). False: none, or unreadable (error).
bool loadAudioBake(void* context, const std::string& dir, std::unique_ptr<AudioBakedData>& out, std::string& error);

// One bake at a time, on a thread of its own.
class AudioBaker {
public:
    ~AudioBaker();
    struct Job {
        void* context = nullptr;  // IPLContext
        bool embree = true;       // traced by Intel's Embree (a device of the bake's own, released after it), when phonon.dll has it
        AudioBakeGeometry geometry;
        AudioBakeSettings settings;
        uint32_t order = 1;         // ambisonic order of the impulse responses
        float savedSeconds = 2.0f;  // the longest the reflection effects play
        std::string dir;
    };
    bool start(Job job);  // false: one is running
    void cancel();
    void stop();  // cancelled, and its thread gone
    bool running() const { return thread_.joinable() && !done_.load(); }
    bool busy() const { return thread_.joinable(); }  // running, or finished and not yet collected
    struct Progress {
        std::string stage;  // scene, probes, reverb, paths, saving
        float fraction = 0.0f;
        uint32_t probes = 0;
    };
    Progress progress() const;
    // Once it is done: true (once), with whether it worked and why not ("Cancelled" when it was).
    bool collect(bool& ok, std::string& error, AudioBakeInfo& info);

private:
    void run(Job job);
    void stage(const char* name, float fraction);
    std::thread thread_;
    std::atomic<bool> done_{false}, cancel_{false};
    std::atomic<void*> context_{nullptr};
    mutable std::mutex lock_;
    Progress progress_;
    bool ok_ = false;
    std::string error_;
    AudioBakeInfo info_;
};

}  // namespace fx
