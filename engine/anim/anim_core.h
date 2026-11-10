#pragma once

// Animation runtime core, ported from Esoterica (MIT, Bobby Anguelov and contributors):
// transforms, skeleton, pose, frame time, clip sampling, root motion and the pose blender.
//
// Conventions. Esoterica writes `a * b` for "a, then b" (DirectXMath quaternion order). Here
// quaternion products are Hamilton (`mul(a, b)` rotates by b first) and transform composition
// is spelled out: combine(local, parent) is Esoterica's `local * parent`. Model space is
// combine(local[i], model[parent[i]]). Scale is uniform, as in Esoterica.

#include "anim/anim_data.h"
#include "core/math.h"

#include <array>
#include <cstdint>
#include <vector>

namespace fx::anim {

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

Quat mul(Quat a, Quat b);  // Hamilton a (x) b: rotate by b, then a
Quat conjugate(Quat q);
Quat normalize(Quat q);
float dot(Quat a, Quat b);
float3 rotate(Quat q, float3 v);
float3 inverseRotate(Quat q, float3 v);
Quat slerp(Quat from, Quat to, float t);      // exact, shortest path (frame interpolation)
Quat fastSlerp(Quat from, Quat to, float t);  // zeux approximation, normalized (pose blending)
Quat fromAxisAngle(float3 axis, float radians);

struct Xform {
    Quat r;
    float3 t = {0, 0, 0};
    float s = 1.0f;

    static Xform identity() { return {}; }
    static Xform zero() { return {Quat{}, {0, 0, 0}, 0.0f}; }  // additive "no change" (scale delta 0)
};

Xform combine(const Xform& local, const Xform& parent);  // local expressed in parent's space
Xform inverse(const Xform& x);
Xform delta(const Xform& from, const Xform& to);          // combine(delta, from) == to
Xform deltaNoScale(const Xform& from, const Xform& to);   // same, scale forced to 1
Xform lerp(const Xform& a, const Xform& b, float t);      // slerp rotation, lerp translation + scale
Xform fastLerp(const Xform& a, const Xform& b, float t);  // fastSlerp rotation, lerp translation + scale
float3 transformPoint(const Xform& x, float3 p);
void toMatrix3x4(const Xform& x, float out[12]);          // rows

// ---- skeleton

class Skeleton {
public:
    void init(const AnimData& data, uint32_t index);
    uint32_t boneCount() const { return uint32_t(parents_.size()); }
    int32_t parent(uint32_t bone) const { return parents_[bone]; }
    const Xform& reference(uint32_t bone) const { return reference_[bone]; }
    const Xform& referenceModel(uint32_t bone) const { return referenceModel_[bone]; }
    const float* inverseBind(uint32_t bone) const { return inverseBind_[bone].data(); }
    int32_t findBone(const char* name) const;
    const char* name(uint32_t bone) const { return names_[bone]; }
    uint32_t index() const { return index_; }

private:
    const AnimData* data_ = nullptr;
    uint32_t index_ = 0;
    std::vector<int32_t> parents_;
    std::vector<Xform> reference_, referenceModel_;
    std::vector<std::array<float, 12>> inverseBind_;
    std::vector<const char*> names_;
};

// ---- pose

class Pose {
public:
    enum class State : uint8_t { Unset, ParentSpace, Reference, Zero, Additive };

    explicit Pose(const Skeleton* skeleton = nullptr) { reset(skeleton); }
    void reset(const Skeleton* skeleton);
    void setToReference();
    void setToZero();  // additive identity: scale delta 0

    const Skeleton* skeleton() const { return skeleton_; }
    uint32_t boneCount() const { return uint32_t(local_.size()); }
    State state() const { return state_; }
    void setState(State s) { state_ = s; }
    bool isAdditive() const { return state_ == State::Additive || state_ == State::Zero; }

    Xform& local(uint32_t bone) { return local_[bone]; }
    const Xform& local(uint32_t bone) const { return local_[bone]; }
    std::vector<Xform>& locals() { return local_; }
    const std::vector<Xform>& locals() const { return local_; }
    const std::vector<Xform>& model() const { return model_; }
    void calculateModelSpace();
    // Model space again for `from` and the bones below it only (the rest as they were).
    void calculateModelSpace(uint32_t from);
    // For the bones from `from` down to `to` (a bone below it) only: the rest below `from` are left
    // stale until a calculateModelSpace (IK, turning a chain toward where its tip goes).
    void calculateModelPath(uint32_t from, uint32_t to);

private:
    const Skeleton* skeleton_ = nullptr;
    std::vector<Xform> local_, model_;
    std::vector<uint8_t> moved_;  // calculateModelSpace(from)'s scratch
    State state_ = State::Unset;
};

// ---- time

// Frame index + fraction to the next frame.
struct FrameTime {
    int32_t frame = 0;
    float fraction = 0.0f;  // [0, 1)

    FrameTime() = default;
    FrameTime(float percent, int32_t frameCount);
    int32_t lower() const { return frame; }
    int32_t upper() const { return fraction > 0.0f ? frame + 1 : frame; }
};

// Wraps (looping) or clamps a percentage; looping keeps exactly 1.0 for the end of a loop.
float wrapPercentage(float p);

// ---- root motion

struct RootMotion {
    const fmt::RootTransform* transforms = nullptr;
    uint32_t count = 0;  // 1 = stationary
    float averageLinearVelocity = 0.0f;

    Xform transform(float percent) const;
    // Root delta between two percentages; handles one loop when to < from.
    Xform deltaBetween(float from, float to) const;
    Xform deltaNoLooping(float from, float to) const;
};

// ---- clip

// A clip's markup (anim/anim_events.h: a model's .fevents file), in percentages through it:
// sync events (the clip's cycle split for synchronized blends: a walk's foot downs) and events
// sampled while it plays (markers and footsteps, instantaneous or with a duration).
struct ClipSyncEvent {
    uint32_t id = 0;  // hashId of its name
    float start = 0.0f;
};
struct ClipEvent {
    enum Kind : uint8_t { Marker, Foot };
    enum Foot : uint8_t { None, Left, Right };
    uint32_t id = 0;
    float start = 0.0f, duration = 0.0f;
    uint8_t kind = Marker, foot = None;
};

class Clip {
public:
    void init(const AnimData& data, uint32_t index, const Skeleton* skeleton);
    const Skeleton* skeleton() const { return skeleton_; }
    float duration() const { return clip_->duration; }
    uint32_t frameCount() const { return clip_->frameCount; }
    bool isAdditive() const { return (clip_->flags & fmt::kClipAdditive) != 0; }
    const char* name() const { return data_->name(clip_->name); }
    const RootMotion& rootMotion() const { return rootMotion_; }
    uint32_t index() const { return index_; }

    // Local pose at a percentage through the clip [0, 1].
    void samplePose(float percent, Pose& out) const;

    // Its markup (sync events sorted by start); graphs read it when they are instantiated.
    void setMarkup(std::vector<ClipSyncEvent> sync, std::vector<ClipEvent> events);
    const std::vector<ClipSyncEvent>& syncEvents() const { return sync_; }
    const std::vector<ClipEvent>& events() const { return events_; }

private:
    void readFrame(uint32_t frame, Xform* out) const;

    const AnimData* data_ = nullptr;
    const fmt::Clip* clip_ = nullptr;
    const fmt::Track* tracks_ = nullptr;
    const uint16_t* poseData_ = nullptr;
    const uint32_t* poseOffsets_ = nullptr;
    const Skeleton* skeleton_ = nullptr;
    RootMotion rootMotion_;
    uint32_t index_ = 0;
    std::vector<ClipSyncEvent> sync_;
    std::vector<ClipEvent> events_;
};

// ---- blending (Esoterica Blender)

enum class BlendMode : uint8_t { Normal, Additive };

// out = blend(source, target, weight) per bone; `boneWeights` (optional, per bone) scale weight.
// Normal: fastSlerp/lerp. Additive: target is an additive pose applied on top of source.
void blendPoses(const Pose& source, const Pose& target, float weight, BlendMode mode, const float* boneWeights,
                Pose& out);
// Turns an additive pose into a full pose on top of the reference pose.
void applyAdditiveToReference(Pose& pose);

Xform blendRootMotion(const Xform& source, const Xform& target, float weight, BlendMode mode);

}  // namespace fx::anim
