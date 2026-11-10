#pragma once

// The Animator's preview pane (the editor's "anim" view): a model of the scene (a library cooked
// into its pak) posed by a graph instance of its own, on the skinning system's floating records
// (no instance slot: it is not in the scene), drawn by PreviewRenderer with an orbit camera. It
// plays in edit mode too: the graph being edited, its parameters as the page sets them, root
// motion kept in place or moving the ground under the character.

#include "anim/anim_core.h"
#include "anim/anim_graph.h"
#include "core/math.h"
#include "render/preview_renderer.h"
#include "ui/ui_layer.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fx {

class AnimationSystem;
class GpuDevice;
class ScenePak;
class SkinningSystem;
class VirtualTextureSystem;

class AnimPreview {
public:
    static constexpr uint32_t kMaxParts = 32;  // skinned meshes of one model (SkinningSystem floating records)

    struct Init {
        const GpuDevice* gpu = nullptr;
        const ScenePak* pak = nullptr;
        SkinningSystem* skinning = nullptr;
        AnimationSystem* animation = nullptr;
        VirtualTextureSystem* vt = nullptr;
    };
    bool init(const Init& init);
    void shutdown();

    // Shows a model (its library source, e.g. "Assets/Hero.glb"): false and why when the scene has
    // no skinned mesh of it (not cooked yet, or rigid).
    bool open(const std::string& model, std::string& error);
    void close();
    bool active() const { return skeleton_ >= 0; }
    const std::string& model() const { return model_; }
    uint32_t skeleton() const { return uint32_t(skeleton_); }

    // The graph it plays (null: the skeleton's first clip, looping). Swapping keeps the parameter
    // values by name.
    void setGraph(std::shared_ptr<const anim::GraphDefinition> graph);
    anim::GraphInstance* graph() { return graph_.get(); }
    void restart();  // the graph (or clip) from its start, parameters kept

    struct Options {
        bool bones = false, grid = true, shadows = true, paused = false;
        bool moveRoot = false;  // root motion moves the character over the ground (else in place)
        float speed = 1.0f;
    };
    Options options;
    void step(float seconds) { pendingStep_ += seconds; }  // while paused

    // Mouse in the pane (`rect`: its pixels): left or right drag orbits, middle or Shift + left
    // drag pans, the wheel zooms, F frames the character. True when it took the event.
    bool handleEvent(const SDL_Event& e, const UiRect& rect, float pixelScaleX, float pixelScaleY);
    // Once a frame: plays the graph (wall-clock dt) and writes the skinning; asks for textures.
    void update(float dt, uint64_t frame);
    void render(SDL_GPUCommandBuffer* cmd, const UiRect& rect);
    SDL_GPUTexture* color() const { return renderer_.color(); }
    void frame();  // the camera fits the character

    float time() const { return time_; }  // seconds played since the last restart
    // The events of every update since the last call (the page's state is sent a few times a second).
    std::vector<anim::SampledEvent> takeEvents() { return std::exchange(recentEvents_, {}); }

private:
    void pose(float dt);

    const ScenePak* pak_ = nullptr;
    SkinningSystem* skinning_ = nullptr;
    AnimationSystem* animation_ = nullptr;
    PreviewRenderer renderer_;
    bool ready_ = false;

    std::string model_;
    int32_t skeleton_ = -1;
    struct Part {
        uint32_t mesh = 0, record = ~0u;  // record: its floating skinning record (~0u: rigid)
        float world[3][4] = {};          // in the model
    };
    std::vector<Part> parts_;
    std::shared_ptr<const anim::GraphDefinition> definition_;
    std::unique_ptr<anim::GraphInstance> graph_;
    anim::Pose pose_;
    int32_t clip_ = -1;
    float time_ = 0.0f, clipTime_ = 0.0f, pendingStep_ = 0.0f;

    // Root motion (moving): the character's yaw and where the ground has moved to under it.
    float yaw_ = 0.0f;
    float2 groundOffset_ = {0, 0};
    float4 bounds_ = {0, 1, 0, 1};  // the character's, preview space

    // Orbit camera around a target.
    float3 target_ = {0, 1, 0};
    float orbitYaw_ = 0.6f, orbitPitch_ = 0.25f, distance_ = 4.0f;
    uint32_t dragButtons_ = 0;
    float2 lastMouse_ = {0, 0};
    bool panning_ = false;
    std::vector<float4> lines_;
    std::vector<anim::SampledEvent> recentEvents_;
};

}  // namespace fx
