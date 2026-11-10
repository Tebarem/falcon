// Ported from Esoterica (MIT): Base/Math (Transform, Quaternion), Base/Encoding/Quantization,
// Engine/Animation (AnimationSkeleton, AnimationPose, AnimationFrameTime, AnimationClip,
// AnimationRootMotion, AnimationBlender). Esoterica's `a * b` (a, then b) is written here as
// Hamilton mul(b, a) for quaternions and combine(a, b) for transforms.

#include "anim/anim_core.h"
#include "core/dmath.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx::anim {

// ---- quaternions

Quat mul(Quat a, Quat b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat conjugate(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
float dot(Quat a, Quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

Quat normalize(Quat q)
{
    const float l = std::sqrt(dot(q, q));
    return l > 1e-12f ? Quat{q.x / l, q.y / l, q.z / l, q.w / l} : Quat{};
}

float3 rotate(Quat q, float3 v)
{
    const float3 u = {q.x, q.y, q.z};
    const float3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

float3 inverseRotate(Quat q, float3 v) { return rotate(conjugate(q), v); }

Quat slerp(Quat from, Quat to, float t)
{
    float cosOmega = dot(from, to);
    const float sign = cosOmega < 0.0f ? -1.0f : 1.0f;
    cosOmega *= sign;
    float s0, s1;
    if (cosOmega < 1.0f - 0.00001f) {
        const float sinOmega = std::sqrt(1.0f - cosOmega * cosOmega);
        const float omega = dmath::atan2(sinOmega, cosOmega);
        s0 = dmath::sin((1.0f - t) * omega) / sinOmega;
        s1 = dmath::sin(t * omega) / sinOmega;
    } else {
        s0 = 1.0f - t;
        s1 = t;
    }
    s1 *= sign;
    // Esoterica leaves this unnormalized; normalizing keeps decoded (slightly off-unit) keys safe.
    return normalize({from.x * s0 + to.x * s1, from.y * s0 + to.y * s1, from.z * s0 + to.z * s1, from.w * s0 + to.w * s1});
}

Quat fastSlerp(Quat q0, Quat q1, float t)
{
    const float d = dot(q0, q1), ad = std::fabs(d);
    const float A = 1.0904f + ad * (-3.2452f + ad * (3.55645f - ad * 1.43519f));
    const float B = 0.848013f + ad * (-1.06021f + ad * 0.215638f);
    const float k = A * (t - 0.5f) * (t - 0.5f) + B;
    const float ot = t + t * (t - 0.5f) * (t - 1.0f) * k;
    const float w0 = 1.0f - ot, w1 = d > 0.0f ? ot : -ot;
    return normalize({q0.x * w0 + q1.x * w1, q0.y * w0 + q1.y * w1, q0.z * w0 + q1.z * w1, q0.w * w0 + q1.w * w1});
}

Quat fromAxisAngle(float3 axis, float radians)
{
    const float s = dmath::sin(radians * 0.5f);
    const float3 a = normalize(axis);
    return {a.x * s, a.y * s, a.z * s, dmath::cos(radians * 0.5f)};
}

// ---- transforms

Xform combine(const Xform& local, const Xform& parent)
{
    Xform r;
    r.r = normalize(mul(parent.r, local.r));
    r.t = rotate(parent.r, local.t * parent.s) + parent.t;
    r.s = local.s * parent.s;
    return r;
}

Xform inverse(const Xform& x)
{
    Xform r;
    r.r = conjugate(x.r);
    r.s = x.s != 0.0f ? 1.0f / x.s : 0.0f;
    r.t = rotate(r.r, x.t * r.s) * -1.0f;
    return r;
}

Xform delta(const Xform& from, const Xform& to)
{
    const Quat inv = conjugate(from.r);
    const float invScale = from.s != 0.0f ? 1.0f / from.s : 0.0f;
    Xform r;
    r.r = mul(inv, to.r);
    r.t = rotate(inv, to.t - from.t) * invScale;
    r.s = to.s * invScale;
    return r;
}

Xform deltaNoScale(const Xform& from, const Xform& to)
{
    const Quat inv = conjugate(from.r);
    Xform r;
    r.r = mul(inv, to.r);
    r.t = rotate(inv, to.t - from.t);
    r.s = 1.0f;
    return r;
}

Xform lerp(const Xform& a, const Xform& b, float t)
{
    return {slerp(a.r, b.r, t), a.t + (b.t - a.t) * t, a.s + (b.s - a.s) * t};
}

Xform fastLerp(const Xform& a, const Xform& b, float t)
{
    return {fastSlerp(a.r, b.r, t), a.t + (b.t - a.t) * t, a.s + (b.s - a.s) * t};
}

float3 transformPoint(const Xform& x, float3 p) { return rotate(x.r, p * x.s) + x.t; }

void toMatrix3x4(const Xform& x, float out[12])
{
    const Quat q = x.r;
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    const float s = x.s;
    const float m[12] = {(1 - 2 * (yy + zz)) * s, 2 * (xy - wz) * s,       2 * (xz + wy) * s,       x.t.x,
                         2 * (xy + wz) * s,       (1 - 2 * (xx + zz)) * s, 2 * (yz - wx) * s,       x.t.y,
                         2 * (xz - wy) * s,       2 * (yz + wx) * s,       (1 - 2 * (xx + yy)) * s, x.t.z};
    std::memcpy(out, m, sizeof(m));
}

// ---- skeleton

void Skeleton::init(const AnimData& data, uint32_t index)
{
    data_ = &data;
    index_ = index;
    const fmt::Skeleton& s = data.skeleton(index);
    const fmt::Bone* bones = data.bones(s);
    parents_.resize(s.boneCount);
    reference_.resize(s.boneCount);
    referenceModel_.resize(s.boneCount);
    inverseBind_.resize(s.boneCount);
    names_.resize(s.boneCount);
    for (uint32_t b = 0; b < s.boneCount; ++b) {
        const fmt::Bone& bone = bones[b];
        parents_[b] = bone.parent;
        reference_[b] = {{bone.rotation[0], bone.rotation[1], bone.rotation[2], bone.rotation[3]},
                         {bone.translation[0], bone.translation[1], bone.translation[2]},
                         bone.scale};
        std::memcpy(inverseBind_[b].data(), bone.inverseBind, sizeof(bone.inverseBind));
        names_[b] = data.name(bone.name);
        referenceModel_[b] = b == 0 ? reference_[0] : combine(reference_[b], referenceModel_[bone.parent]);
    }
}

int32_t Skeleton::findBone(const char* name) const
{
    for (uint32_t b = 0; b < names_.size(); ++b)
        if (std::strcmp(names_[b], name) == 0)
            return int32_t(b);
    return -1;
}

// ---- pose

void Pose::reset(const Skeleton* skeleton)
{
    skeleton_ = skeleton;
    local_.assign(skeleton ? skeleton->boneCount() : 0, Xform{});
    model_.clear();
    state_ = State::Unset;
}

void Pose::setToReference()
{
    for (uint32_t b = 0; b < local_.size(); ++b)
        local_[b] = skeleton_->reference(b);
    model_.clear();
    state_ = State::Reference;
}

void Pose::setToZero()
{
    std::fill(local_.begin(), local_.end(), Xform::zero());
    model_.clear();
    state_ = State::Zero;
}

void Pose::calculateModelSpace()
{
    model_.resize(local_.size());
    if (local_.empty())
        return;
    model_[0] = local_[0];
    for (uint32_t b = 1; b < local_.size(); ++b)
        model_[b] = combine(local_[b], model_[skeleton_->parent(b)]);
}

void Pose::calculateModelSpace(uint32_t from)
{
    const int32_t parent = from < local_.size() ? skeleton_->parent(from) : -1;
    if (parent < 0 || model_.size() != local_.size()) {
        calculateModelSpace();
        return;
    }
    // Bones come after their parents: those after it whose parent moved, moved.
    model_[from] = combine(local_[from], model_[uint32_t(parent)]);
    moved_.assign(local_.size(), 0);
    moved_[from] = 1;
    for (uint32_t b = from + 1; b < local_.size(); ++b) {
        const int32_t p = skeleton_->parent(b);
        if (p >= int32_t(from) && moved_[uint32_t(p)]) {
            model_[b] = combine(local_[b], model_[uint32_t(p)]);
            moved_[b] = 1;
        }
    }
}

void Pose::calculateModelPath(uint32_t from, uint32_t to)
{
    if (model_.size() != local_.size() || to >= local_.size()) {
        calculateModelSpace();
        return;
    }
    uint32_t path[256];
    uint32_t n = 0;
    for (int32_t b = int32_t(to); b >= 0 && n < 256; b = skeleton_->parent(uint32_t(b))) {
        path[n++] = uint32_t(b);
        if (uint32_t(b) == from)
            break;
    }
    for (uint32_t i = n; i-- > 0;) {
        const int32_t p = skeleton_->parent(path[i]);
        model_[path[i]] = p >= 0 ? combine(local_[path[i]], model_[uint32_t(p)]) : local_[path[i]];
    }
}

// ---- time

float wrapPercentage(float p)
{
    if (p >= 0.0f && p <= 1.0f)
        return p;
    float whole;
    float frac = std::modf(p, &whole);
    if (frac < 0.0f)
        frac += 1.0f;  // (Esoterica's Clamp gives 1 - frac here, > 1)
    return frac;
}

FrameTime::FrameTime(float percent, int32_t frameCount)
{
    const int32_t last = frameCount - 1;
    if (percent <= 0.0f || last <= 0) {
        frame = 0;
        fraction = 0.0f;
    } else if (percent >= 1.0f) {
        frame = last;
        fraction = 0.0f;
    } else {
        float whole = 0.0f;
        fraction = std::modf(percent * float(last), &whole);
        if (fraction < 1e-4f)
            fraction = 0.0f;
        frame = int32_t(whole);
    }
}

// ---- root motion

Xform RootMotion::transform(float percent) const
{
    auto at = [&](uint32_t i) {
        const fmt::RootTransform& r = transforms[i];
        return Xform{{r.rotation[0], r.rotation[1], r.rotation[2], r.rotation[3]},
                     {r.translation[0], r.translation[1], r.translation[2]},
                     r.scale};
    };
    if (count <= 1)
        return count ? at(0) : Xform{};
    const FrameTime ft(percent, int32_t(count));
    if (ft.fraction == 0.0f)
        return at(uint32_t(ft.frame));
    return lerp(at(uint32_t(ft.frame)), at(uint32_t(ft.frame) + 1), ft.fraction);
}

Xform RootMotion::deltaNoLooping(float from, float to) const
{
    return count <= 1 ? Xform{} : deltaNoScale(transform(from), transform(to));
}

Xform RootMotion::deltaBetween(float from, float to) const
{
    if (count <= 1)
        return {};
    if (from <= to)
        return deltaNoScale(transform(from), transform(to));
    const Xform pre = deltaNoScale(transform(from), transform(1.0f));
    const Xform post = deltaNoScale(transform(0.0f), transform(to));
    return combine(post, pre);  // pre first, then post
}

// ---- clip

void Clip::init(const AnimData& data, uint32_t index, const Skeleton* skeleton)
{
    data_ = &data;
    index_ = index;
    clip_ = &data.clip(index);
    skeleton_ = skeleton;
    tracks_ = data.at<fmt::Track>(clip_->tracks);
    poseData_ = data.at<uint16_t>(clip_->poseData);
    poseOffsets_ = data.at<uint32_t>(clip_->poseOffsets);
    rootMotion_.transforms = data.at<fmt::RootTransform>(clip_->rootMotion);
    rootMotion_.count = clip_->rootMotionCount;
    rootMotion_.averageLinearVelocity = clip_->averageLinearVelocity;
}

void Clip::setMarkup(std::vector<ClipSyncEvent> sync, std::vector<ClipEvent> events)
{
    std::stable_sort(sync.begin(), sync.end(), [](const ClipSyncEvent& a, const ClipSyncEvent& b) { return a.start < b.start; });
    sync_ = std::move(sync);
    events_ = std::move(events);
}

namespace {

constexpr float kOneDivSqrt2 = 0.70710678118654752f;

Quat decodeRotation(const uint16_t* d)
{
    const float scale = (2.0f * kOneDivSqrt2) / float(0x7fff);
    const float v0 = float(d[0] & 0x7fff) * scale - kOneDivSqrt2;
    const float v1 = float(d[1] & 0x7fff) * scale - kOneDivSqrt2;
    const float v2 = float(d[2]) * scale - kOneDivSqrt2;
    const float v3 = std::sqrt(std::max(0.0f, 1.0f - (v0 * v0 + v1 * v1 + v2 * v2)));
    switch ((d[0] >> 14 & 2) | d[1] >> 15) {
    case 0: return {v3, v0, v1, v2};
    case 1: return {v0, v3, v1, v2};
    case 2: return {v0, v1, v3, v2};
    default: return {v0, v1, v2, v3};
    }
}

float decodeFloat(uint16_t e, const fmt::FloatRange& r) { return float(e) / 65535.0f * r.length + r.start; }

}  // namespace

void Clip::readFrame(uint32_t frame, Xform* out) const
{
    const uint16_t* p = poseData_ + poseOffsets_[frame];
    const uint32_t bones = skeleton_->boneCount();
    for (uint32_t b = 0; b < bones; ++b) {
        const fmt::Track& t = tracks_[b];
        Xform& x = out[b];
        if (t.staticRotation) {
            x.r = {t.constantRotation[0], t.constantRotation[1], t.constantRotation[2], t.constantRotation[3]};
        } else {
            x.r = decodeRotation(p);
            p += 3;
        }
        if (t.staticTranslation) {
            x.t = {t.translation[0].start, t.translation[1].start, t.translation[2].start};
        } else {
            x.t = {decodeFloat(p[0], t.translation[0]), decodeFloat(p[1], t.translation[1]), decodeFloat(p[2], t.translation[2])};
            p += 3;
        }
        if (t.staticScale) {
            x.s = t.scale.start;
        } else {
            x.s = decodeFloat(p[0], t.scale);
            p += 1;
        }
    }
}

void Clip::samplePose(float percent, Pose& out) const
{
    const FrameTime ft(percent, int32_t(clip_->frameCount));
    std::vector<Xform>& locals = out.locals();
    readFrame(uint32_t(ft.lower()), locals.data());
    if (ft.fraction > 0.0f) {
        thread_local std::vector<Xform> upper;
        upper.resize(locals.size());
        readFrame(uint32_t(ft.upper()), upper.data());
        for (size_t b = 0; b < locals.size(); ++b)
            locals[b] = lerp(locals[b], upper[b], ft.fraction);
    }
    out.setState(isAdditive() ? Pose::State::Additive : Pose::State::ParentSpace);
}

// ---- blending

namespace {

Xform blendAdditive(const Xform& base, const Xform& add, float w)
{
    // Esoterica: SLerp(base, add * base, w) == Hamilton slerp(base, base (x) add, w); t/s: base + w * add.
    return {slerp(base.r, mul(base.r, add.r), w), base.t + add.t * w, base.s + add.s * w};
}

}  // namespace

void blendPoses(const Pose& source, const Pose& target, float weight, BlendMode mode, const float* boneWeights, Pose& out)
{
    const bool additive = mode == BlendMode::Additive;
    const Pose::State finalState =
        source.isAdditive() && target.isAdditive() ? Pose::State::Additive : Pose::State::ParentSpace;
    const uint32_t bones = out.boneCount();
    if (!boneWeights && weight <= 0.0f) {
        if (&out != &source)
            out.locals() = source.locals();
    } else if (!boneWeights && !additive && weight >= 1.0f) {
        if (&out != &target)
            out.locals() = target.locals();
    } else {
        for (uint32_t b = 0; b < bones; ++b) {
            const float w = boneWeights ? weight * boneWeights[b] : weight;
            const Xform& s = source.local(b);
            const Xform& t = target.local(b);
            if (w <= 0.0f)
                out.local(b) = s;
            else if (!additive && w >= 1.0f)
                out.local(b) = t;
            else
                out.local(b) = additive ? blendAdditive(s, t, w) : fastLerp(s, t, w);
        }
    }
    out.setState(finalState);
}

void applyAdditiveToReference(Pose& pose)
{
    for (uint32_t b = 0; b < pose.boneCount(); ++b)
        pose.local(b) = blendAdditive(pose.skeleton()->reference(b), pose.local(b), 1.0f);
    pose.setState(Pose::State::ParentSpace);
}

Xform blendRootMotion(const Xform& source, const Xform& target, float w, BlendMode mode)
{
    if (w <= 0.0f)
        return source;
    if (w >= 1.0f && mode == BlendMode::Normal)
        return target;
    Xform r;
    if (mode == BlendMode::Additive) {
        r.r = slerp(source.r, mul(source.r, target.r), w);
        r.t = target.t * w + source.t;
    } else {
        r.r = fastSlerp(source.r, target.r, w);
        r.t = source.t + (target.t - source.t) * w;
    }
    r.s = 1.0f;
    return r;
}

}  // namespace fx::anim
