#include "anim/bone_entities.h"

#include "anim/animation_system.h"
#include "components.gen.h"
#include "render/skinning.h"
#include "scene/scene.h"

#include <cstring>

namespace fx {

namespace {

void toRows(const anim::Xform& x, float out[3][4]) { anim::toMatrix3x4(x, &out[0][0]); }

}  // namespace

const BoneEntities::Expanded* BoneEntities::expand(Scene& scene, flecs::entity root, const AnimationSystem& animation,
                                                   const SkinningSystem& skinning)
{
    if (const Expanded* x = find(root.id()))
        return x;
    const std::vector<uint32_t> records = animation.recordsUnder(scene.world(), root.id());
    if (records.empty())
        return nullptr;
    const uint32_t record = records.front();
    const anim::Pose* pose = animation.pose(record);
    const anim::Skeleton* skeleton = pose ? pose->skeleton() : nullptr;
    const flecs::entity mesh = scene.entity(skinning.instanceOfRecord(record));
    const WorldTransform* rootWorld = root.try_get<WorldTransform>();
    const WorldTransform* meshWorld = mesh ? mesh.try_get<WorldTransform>() : nullptr;
    if (!skeleton || !rootWorld || !meshWorld)
        return nullptr;

    // The group in the mesh's place under the root (the bones are in the mesh's space).
    float inverse[3][4], local[3][4];
    affineInverse(rows3x4(rootWorld->rows), inverse);
    affineMultiply(inverse, rows3x4(meshWorld->rows), local);
    Expanded x;
    x.root = root.id();
    x.record = record;
    x.skeleton = skeleton;
    flecs::entity group = scene.spawnTransform(rows3x4(meshWorld->rows));
    group.set<Bone>({0, 1});
    scene.setLocal(group, root, local);
    x.group = group.id();
    x.bones.resize(skeleton->boneCount());
    for (uint32_t b = 0; b < skeleton->boneCount(); ++b) {
        const int32_t parent = skeleton->parent(b);
        const flecs::entity up = parent < 0 ? group : flecs::entity(scene.world(), x.bones[uint32_t(parent)]);
        float rows[3][4];
        toRows(pose->locals().empty() ? skeleton->reference(b) : pose->local(b), rows);
        flecs::entity bone = scene.spawnTransform(rows);
        bone.set<Bone>({b, 0});
        scene.setLocal(bone, up, rows);
        x.bones[b] = bone.id();
    }
    expanded_.push_back(std::move(x));
    scene.propagate();
    return &expanded_.back();
}

const BoneEntities::Expanded* BoneEntities::find(flecs::entity_t root) const
{
    for (const Expanded& x : expanded_)
        if (x.root == root)
            return &x;
    return nullptr;
}

flecs::entity_t BoneEntities::bone(flecs::entity_t root, std::string_view name) const
{
    const Expanded* x = find(root);
    if (!x)
        return 0;
    if (name.empty())
        return x->group;
    for (uint32_t b = 0; b < x->bones.size(); ++b)
        if (name == x->skeleton->name(b))
            return x->bones[b];
    return 0;
}

flecs::entity_t BoneEntities::named(const flecs::world& world, std::string_view name) const
{
    for (const Expanded& x : expanded_) {
        if (!world.is_alive(x.group) || !flecs::entity(world, x.group).enabled())
            continue;
        for (uint32_t b = 0; b < x.bones.size(); ++b)
            if (name == x.skeleton->name(b))
                return x.bones[b];
    }
    return 0;
}

bool BoneEntities::update(flecs::world& world, const AnimationSystem& animation)
{
    bool moved = false;
    for (const Expanded& x : expanded_) {
        if (!world.is_alive(x.group) || !flecs::entity(world, x.group).enabled())
            continue;
        const anim::Pose* pose = animation.pose(x.record);
        if (!pose || pose->boneCount() != x.bones.size())
            continue;
        for (uint32_t b = 0; b < x.bones.size(); ++b) {
            if (!world.is_alive(x.bones[b]))
                continue;
            LocalTransform local;
            anim::toMatrix3x4(pose->local(b), local.rows);
            flecs::entity e(world, x.bones[b]);
            const LocalTransform* now = e.try_get<LocalTransform>();
            if (now && std::memcmp(now->rows, local.rows, sizeof(local.rows)) == 0)
                continue;
            e.set<LocalTransform>(local);
            moved = true;
        }
    }
    return moved;
}

}  // namespace fx
