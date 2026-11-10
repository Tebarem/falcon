#pragma once

// Rigid node animation at runtime: entities linked to a baked track (ScenePak::NodeTrack: a
// model's moving nodes and cameras, or the scene's own) follow it while the scene plays, looping.
// A child (an entity placed under its model's root) takes the track as its LocalTransform, a
// root as its world transform.

#include "assets/scene_pak.h"

#include <flecs.h>

#include <vector>

namespace fx {

class Scene;

class NodeAnimator {
public:
    struct Link {
        flecs::entity_t entity = 0;
        const ScenePak::NodeTrack* track = nullptr;
    };
    void set(std::vector<Link> links, float fps)
    {
        links_ = std::move(links);
        fps_ = fps;
        time_ = 0.0f;
    }
    void clear() { links_.clear(); }
    bool active() const { return !links_.empty(); }
    // Moves the linked entities to `dt` seconds later (0: where they are).
    void update(Scene& scene, float dt);
    float time() const { return time_; }

private:
    std::vector<Link> links_;
    float fps_ = 30.0f, time_ = 0.0f;
};

}  // namespace fx
