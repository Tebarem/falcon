#pragma once

// Inverse kinematics on a pose, after the graph has made it and before it is skinned: a limb of
// two bones reaching a point (a foot onto the ground, a hand onto a grip) and a chain of bones
// turning toward a point (the spine, neck and head looking or aiming). Everything is in the pose's
// model space; each solve writes the bones' parent-space locals and brings the model space up to
// date.

#include "anim/anim_core.h"

#include <cstdint>
#include <vector>

namespace fx::anim {

Quat fromTo(float3 from, float3 to);  // the shortest rotation taking direction `from` onto `to`

// A bone's model-space rotation set (its parent's model space unchanged); updates model space.
void setModelRotation(Pose& pose, uint32_t bone, Quat rotation);

// The limb upper -> mid -> end (end's parent is mid, mid's is upper) bends so that end's joint
// reaches `target` (as near as the bones' lengths allow), turning upper and mid only, in the plane
// the limb bends in now (or, held straight, the one it bends in at rest). `weight` blends from the
// pose (0) to the solution (1). `keepEnd`: the end bone keeps its model-space rotation (a hand on a
// grip) rather than turning with the limb.
void solveTwoBone(Pose& pose, uint32_t upper, uint32_t mid, uint32_t end, float3 target, float weight, bool keepEnd);

// A chain of bones (root first, each an ancestor of the next) turns so that the last one's
// `forward` (a direction in that bone's own space) points from its joint at `target`. Each bone
// takes a share of the turn (`shares`, one per bone, relative); the turn is limited to
// `maxAngle` radians from where the pose points. `weight` blends from the pose to the solution.
void solveLook(Pose& pose, const std::vector<uint32_t>& chain, const std::vector<float>& shares, float3 forward,
               float3 target, float maxAngle, float weight);

// The chain turns (as solveLook) so that `effector`'s `forward` (in its own space; the effector
// anywhere below the chain, a hand) points from its joint at `target`. The effector moves as the
// chain turns, so the turn is found again from there `passes` times.
void solveAim(Pose& pose, const std::vector<uint32_t>& chain, const std::vector<float>& shares, uint32_t effector,
              float3 forward, float3 target, float maxAngle, float weight, int passes);

// A held weapon: `bone` (the hand holding it) brought to `at` by its limb (its parent and
// grandparent), then turned so that its `forward` (in its own space) points at `target`; `support`
// (the other hand, -1: none) keeps its grip on it (where it was relative to the bone), its own limb
// reaching there. `weight` blends from the pose.
void solveHold(Pose& pose, uint32_t bone, float3 forward, float3 at, float3 target, int32_t support, float weight);

// The bones to look with, ending at `tip`: it and its ancestors named like a spine, neck, chest or
// head (case-insensitively), root first; just `tip` when none are.
std::vector<uint32_t> lookChain(const Skeleton& skeleton, uint32_t tip);

// The bones to aim `bone` (a hand) with: its nearest ancestors named like a spine or chest, root
// first (the spine it hangs from); empty when none are.
std::vector<uint32_t> aimChain(const Skeleton& skeleton, uint32_t bone);

}  // namespace fx::anim
