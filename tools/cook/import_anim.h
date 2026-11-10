#pragma once

// glTF skins and animations -> SrcSkeleton / SrcClip (see import_anim.cpp).

#include "cook.h"

#include <cgltf.h>

#include <unordered_map>
#include <vector>

namespace fx::cook {

// Skin of a mesh: the skeleton it binds to and glTF joint index -> skeleton bone.
struct MeshSkinBinding {
    int32_t skeleton = -1;
    std::vector<uint16_t> jointToBone;
    float armature[16];  // world transform of the skeleton's model space
};

// One SrcSkeleton per glTF skin; returns the binding of every skinned mesh (first node wins).
std::unordered_map<const cgltf_mesh*, MeshSkinBinding> importSkeletons(const cgltf_data* data, SrcScene& scene);

// Joints/weights of a primitive into its vertices (remapped to skeleton bones, normalized).
void importSkinWeights(const cgltf_primitive& prim, const MeshSkinBinding& binding, SrcPrimitive& out);

// Every animation that targets a skeleton's joints, sampled at ~fps frames per second.
void importClips(const cgltf_data* data, SrcScene& scene, float fps);

// Every animation channel on other nodes (rigid animation), all played together: the world
// transform of each mesh node and camera that moves, sampled at fps (SrcScene::nodeTracks).
void importNodeTracks(const cgltf_data* data, SrcScene& scene, float fps);

}  // namespace fx::cook
