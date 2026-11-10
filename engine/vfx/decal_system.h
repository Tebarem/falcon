#pragma once

// Decals: the Decal components of the scene, and decals placed while a game runs (Decals.Place:
// bullet holes), a pool of the most recent ones that fade at the end of their lifetime. Each is a
// box projecting its material along its -Y onto what is inside it; the resolve applies them
// (render/decal_renderer.h, shaders/visbuffer_resolve.slang), with parallax through the material's
// height map.

#include <flecs.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fx {

class Scene;

// One decal as the renderer takes it.
struct DecalInstance {
    float world[3][4];  // its box: columns are its axes times its size (x and z across, y its depth), then its centre
    uint32_t material = 0;  // vfxAssetId of its .fmat
    uint32_t flags = 0;     // vfx_components.h kDecal*
    float tint[4] = {1, 1, 1, 1};
    float angleFade = 0.3f, edgeFade = 0.1f, cavity = 0.5f, parallaxDepth = 0.02f;
    uint32_t steps = 16;
    int32_t order = 0;
    uint64_t sequence = 0;  // placed decals: newer on top of older at the same order
};

class DecalSystem {
public:
    static constexpr uint32_t kMaxPlaced = 2048;

    // A decal placed by a script: at `point`, +Y along `normal`, turned `angle` degrees about it,
    // `size` across and `depth` deep, for `lifetime` seconds (0: until cleared), following `attach`.
    bool place(Scene& scene, const std::string& material, const float point[3], const float normal[3], float size, float depth,
               float angle, float lifetime, flecs::entity attach);
    void clear() { placed_.clear(); }
    // A frame: placed decals age (those past their lifetime go), attached ones follow.
    void update(Scene& scene, float dt);
    // The scene's Decal components and the placed decals.
    void gather(Scene& scene, std::vector<DecalInstance>& out) const;
    uint32_t placedCount() const { return uint32_t(placed_.size()); }

private:
    struct Placed {
        DecalInstance decal;
        float age = 0.0f, lifetime = 0.0f;
        flecs::entity_t attach = 0;
        float local[3][4] = {};  // in the attached entity's space
    };
    std::vector<Placed> placed_;
    uint64_t sequence_ = 0;
};

}  // namespace fx
