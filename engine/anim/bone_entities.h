#pragma once

// Characters' skeletons as entities (the editor's Expand Bones; the edits save it as the model
// root's "bones"). Under the model root: a "Skeleton" group entity in the leading skinned mesh's
// place (bones are in its space), and below it one entity per bone, parented as the skeleton is,
// each with a Bone component. Every frame, after the animation update, each bone's LocalTransform
// takes its character's pose, so whatever hangs under a bone (an empty, a light, a sound, a mesh)
// follows the animation. A skeleton whose group is disabled (collapsed, undone, deleted) is not
// driven.

#include <flecs.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace fx {

namespace anim {
class Skeleton;
}
class AnimationSystem;
class Scene;
class SkinningSystem;

class BoneEntities {
public:
    struct Expanded {
        flecs::entity_t root = 0, group = 0;
        uint32_t record = 0;  // the character's skinning record (the one its pose is read from)
        const anim::Skeleton* skeleton = nullptr;
        std::vector<flecs::entity_t> bones;  // by bone index
    };

    // `root`'s character (the first skinned record of it and its descendants) as entities: the
    // group and its bones, new (enabled, posed as now) or the ones expanded already. nullptr when
    // it has no skinned mesh.
    const Expanded* expand(Scene& scene, flecs::entity root, const AnimationSystem& animation, const SkinningSystem& skinning);
    const Expanded* find(flecs::entity_t root) const;
    // The bone of `root`'s skeleton named `name` ("": the group); 0 when none.
    flecs::entity_t bone(flecs::entity_t root, std::string_view name) const;
    // The first bone named `name` of any enabled skeleton (a game's Entity.Find); 0 when none.
    flecs::entity_t named(const flecs::world& world, std::string_view name) const;
    // Each enabled skeleton's bones from its character's pose. True when a bone moved (then
    // Scene::propagate, so what hangs under them follows this frame).
    bool update(flecs::world& world, const AnimationSystem& animation);
    bool empty() const { return expanded_.empty(); }
    const std::vector<Expanded>& all() const { return expanded_; }

private:
    std::vector<Expanded> expanded_;
};

}  // namespace fx
