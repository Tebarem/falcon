// Skeletons and clips -> the Animation section (engine/anim/anim_format.h).
//
// Clip compression is Esoterica's (MIT, AnimationClipCompiler::TransferAndCompressAnimationData):
// per bone and frame, a 48-bit smallest-three rotation, a 48-bit translation quantized to the
// track's [min, max] and a 16-bit uniform scale, each omitted when constant over the clip.
// No error metric or keyframe reduction: every frame of a moving track is stored.
// Differences from the original: static values come from the clip's own first frame and sync
// markers are stored as percentages (the original stores seconds); root motion is bone 0's
// track (identity here: our bone 0 is the synthetic armature root).

#include "cook.h"

#include "anim/anim_core.h"
#include "anim/anim_format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fx::cook {

namespace {

namespace fmt = anim::fmt;

constexpr float kOneDivSqrt2 = 0.70710678118654752f;
constexpr float kRotationEpsilon = 1e-4f;   // radians (Math::LargeEpsilon)
constexpr float kRangeEpsilon = 1e-6f;      // Math::IsNearZero
constexpr float kStaticRangeLength = 0.1f;  // dummy length of static tracks

uint16_t encodeFloat(float value, float start, float length)
{
    const float n = std::clamp((value - start) / length, 0.0f, 1.0f);
    return uint16_t(n * 65535.0f + 0.5f);
}

// Esoterica EncodedQuaternion: drop the largest component (made positive), store the other
// three in 15 bits over [-1/sqrt2, 1/sqrt2]; the dropped index's high bit is data0 bit 15 and
// its low bit data1 bit 15.
void encodeQuaternion(const float q[4], uint16_t out[3])
{
    uint32_t largest = 0;
    for (uint32_t i = 1; i < 4; ++i)
        if (std::fabs(q[i]) > std::fabs(q[largest]))
            largest = i;
    const float sign = q[largest] < 0.0f ? -1.0f : 1.0f;
    const float range = 2.0f * kOneDivSqrt2, scale = float(0x7fff) / range;
    auto quantize = [&](float c) {
        return uint16_t(std::clamp(std::lround((c * sign + kOneDivSqrt2) * scale), 0l, 0x7fffl));
    };
    uint16_t v[3];
    for (uint32_t i = 0, k = 0; i < 4; ++i)
        if (i != largest)
            v[k++] = quantize(q[i]);
    out[0] = uint16_t(v[0] | (largest & 2 ? 0x8000 : 0));
    out[1] = uint16_t(v[1] | (largest & 1 ? 0x8000 : 0));
    out[2] = v[2];
}

float quatDistance(const float a[4], const float b[4])
{
    const float d = std::clamp(std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]), 0.0f, 1.0f);
    return 2.0f * std::acos(d);
}

void normalized(const float in[4], float out[4])
{
    const float l = std::sqrt(in[0] * in[0] + in[1] * in[1] + in[2] * in[2] + in[3] * in[3]);
    for (int i = 0; i < 4; ++i)
        out[i] = l > 0.0f ? in[i] / l : (i == 3 ? 1.0f : 0.0f);
}

// Esoterica transforms carry one uniform scale.
float uniformScale(const SrcTransform& t) { return (t.s[0] + t.s[1] + t.s[2]) / 3.0f; }

struct Writer {
    std::vector<uint8_t> bytes;
    template <typename T>
    uint32_t put(const T* data, size_t count)
    {
        bytes.resize((bytes.size() + 3) & ~size_t(3));
        const uint32_t at = uint32_t(bytes.size());
        const auto* p = reinterpret_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + count * sizeof(T));
        return at;
    }
    template <typename T>
    T& at(uint32_t offset) { return *reinterpret_cast<T*>(bytes.data() + offset); }
};

}  // namespace

std::vector<uint8_t> buildAnimation(const SrcScene& scene, const std::vector<CompiledGraph>& graphs)
{
    if (scene.skeletons.empty())
        return {};
    Writer w;
    w.bytes.assign(sizeof(fmt::Header), 0);
    std::string strings;
    auto name = [&](const std::string& s) {
        const uint32_t at = uint32_t(strings.size());
        strings += s;
        strings += '\0';
        return at;
    };

    std::vector<fmt::Skeleton> skeletons;
    for (const SrcSkeleton& sk : scene.skeletons) {
        std::vector<fmt::Bone> bones(sk.parents.size());
        for (size_t b = 0; b < bones.size(); ++b) {
            fmt::Bone& o = bones[b];
            o.name = name(sk.boneNames[b]);
            o.parent = sk.parents[b];
            normalized(sk.reference[b].r, o.rotation);
            std::memcpy(o.translation, sk.reference[b].t, sizeof(o.translation));
            o.scale = uniformScale(sk.reference[b]);
            const auto& m = sk.inverseBind[b];  // column-major 4x4 -> 3x4 rows
            for (int row = 0; row < 3; ++row)
                for (int c = 0; c < 4; ++c)
                    o.inverseBind[row * 4 + c] = m[c * 4 + row];
        }
        fmt::Skeleton s = {};
        s.name = name(sk.name);
        s.boneCount = uint32_t(bones.size());
        s.bones = w.put(bones.data(), bones.size());
        skeletons.push_back(s);
    }

    std::vector<fmt::Clip> clips;
    for (const SrcClip& clip : scene.clips) {
        const SrcSkeleton& sk = scene.skeletons[clip.skeleton];
        const size_t bones = sk.parents.size();
        const uint32_t frames = clip.frameCount;
        auto local = [&](uint32_t f, size_t b) -> const SrcTransform& { return clip.frames[f * bones + b]; };

        // Root motion: bone 0's local track re-zeroed to the origin at frame 0; the root bone
        // itself is stored as identity.
        std::vector<fmt::RootTransform> root(frames);
        for (uint32_t f = 0; f < frames; ++f) {
            const SrcTransform& t = local(f, 0);
            normalized(t.r, root[f].rotation);
            for (int a = 0; a < 3; ++a)
                root[f].translation[a] = t.t[a] - local(0, 0).t[a];
            root[f].scale = 1.0f;
        }
        bool hasRootMotion = false;
        float distance = 0.0f;
        for (uint32_t f = 1; f < frames; ++f) {
            float d2 = 0.0f;
            for (int a = 0; a < 3; ++a)
                d2 += (root[f].translation[a] - root[f - 1].translation[a]) * (root[f].translation[a] - root[f - 1].translation[a]);
            distance += std::sqrt(d2);
            hasRootMotion |= d2 > 1e-12f || quatDistance(root[f].rotation, root[f - 1].rotation) > 0.0174533f;
        }

        // Track ranges and static detection over the clip.
        std::vector<fmt::Track> tracks(bones);
        uint32_t readOffset = 0;
        for (size_t b = 0; b < bones; ++b) {
            fmt::Track& t = tracks[b];
            float q0[4];
            normalized(local(0, b).r, q0);
            bool rotationStatic = true;
            float lo[4] = {1e30f, 1e30f, 1e30f, 1e30f}, hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
            for (uint32_t f = 0; f < frames; ++f) {
                const SrcTransform& x = b == 0 ? SrcTransform{} : local(f, b);
                float q[4];
                normalized(x.r, q);
                if (b != 0 && quatDistance(q0, q) > kRotationEpsilon)
                    rotationStatic = false;
                const float v[4] = {x.t[0], x.t[1], x.t[2], uniformScale(x)};
                for (int k = 0; k < 4; ++k) {
                    lo[k] = std::min(lo[k], v[k]);
                    hi[k] = std::max(hi[k], v[k]);
                }
            }
            t.readOffset = readOffset;
            t.staticRotation = rotationStatic;
            if (b == 0)
                t.constantRotation[3] = 1.0f;
            else
                std::memcpy(t.constantRotation, q0, sizeof(q0));
            const bool translationStatic = hi[0] - lo[0] <= kRangeEpsilon && hi[1] - lo[1] <= kRangeEpsilon &&
                                           hi[2] - lo[2] <= kRangeEpsilon;
            t.staticTranslation = translationStatic;
            for (int a = 0; a < 3; ++a) {
                const float len = hi[a] - lo[a];
                t.translation[a] = {translationStatic ? (b == 0 ? 0.0f : local(0, b).t[a]) : lo[a],
                                    translationStatic || len <= kRangeEpsilon ? kStaticRangeLength : len};
            }
            t.staticScale = hi[3] - lo[3] <= kRangeEpsilon;
            t.scale = {t.staticScale ? (b == 0 ? 1.0f : uniformScale(local(0, b))) : lo[3],
                       t.staticScale ? kStaticRangeLength : hi[3] - lo[3]};
            readOffset += (t.staticRotation ? 0 : 3) + (t.staticTranslation ? 0 : 3) + (t.staticScale ? 0 : 1);
        }

        std::vector<uint16_t> data;
        std::vector<uint32_t> offsets(frames);
        for (uint32_t f = 0; f < frames; ++f) {
            offsets[f] = uint32_t(data.size());
            for (size_t b = 0; b < bones; ++b) {
                const fmt::Track& t = tracks[b];
                const SrcTransform& x = local(f, b);
                if (!t.staticRotation) {
                    float q[4];
                    normalized(x.r, q);
                    uint16_t e[3];
                    encodeQuaternion(q, e);
                    data.insert(data.end(), e, e + 3);
                }
                if (!t.staticTranslation)
                    for (int a = 0; a < 3; ++a)
                        data.push_back(encodeFloat(x.t[a], t.translation[a].start, t.translation[a].length));
                if (!t.staticScale)
                    data.push_back(encodeFloat(uniformScale(x), t.scale.start, t.scale.length));
            }
        }

        fmt::Clip c = {};
        c.name = name(clip.name);
        c.skeleton = clip.skeleton;
        c.frameCount = frames;
        c.duration = clip.duration;
        c.tracks = w.put(tracks.data(), tracks.size());
        c.poseData = w.put(data.data(), data.size());
        c.poseOffsets = w.put(offsets.data(), offsets.size());
        if (!hasRootMotion)
            root.resize(1);  // stationary: one transform, deltas are identity
        c.rootMotion = w.put(root.data(), root.size());
        c.rootMotionCount = uint32_t(root.size());
        c.syncEvents = 0;
        c.syncEventCount = 0;  // no sync markers in glTF: the runtime uses one event over [0, 1)
        c.averageLinearVelocity = hasRootMotion && clip.duration > 0.0f ? distance / clip.duration : 0.0f;
        const fmt::RootTransform& last = root.back();
        std::memcpy(c.totalDelta, last.rotation, sizeof(last.rotation));
        std::memcpy(c.totalDelta + 4, last.translation, sizeof(last.translation));
        c.totalDelta[7] = 1.0f;
        clips.push_back(c);
    }

    std::vector<fmt::Graph> graphRecords;
    for (const CompiledGraph& g : graphs) {
        fmt::Graph r = {};
        r.name = name(g.name);
        r.skeleton = g.skeleton;
        r.nodeCount = uint32_t(g.nodes.size());
        r.nodes = w.put(g.nodes.data(), g.nodes.size());
        r.pool = w.put(g.pool.data(), g.pool.size());
        r.poolCount = uint32_t(g.pool.size());
        std::vector<uint32_t> names;
        for (const std::string& p : g.parameterNames)
            names.push_back(name(p));
        r.parameterNames = w.put(names.data(), names.size());
        r.parameterCount = uint16_t(names.size());
        r.root = g.root;
        graphRecords.push_back(r);
    }

    fmt::Header h = {};
    std::memcpy(h.magic, fmt::kMagic, 4);
    h.version = fmt::kVersion;
    h.skeletonCount = uint32_t(skeletons.size());
    h.clipCount = uint32_t(clips.size());
    h.skeletons = w.put(skeletons.data(), skeletons.size());
    h.clips = w.put(clips.data(), clips.size());
    h.graphCount = uint32_t(graphRecords.size());
    h.graphs = w.put(graphRecords.data(), graphRecords.size());
    h.strings = w.put(strings.data(), strings.size());
    w.at<fmt::Header>(0) = h;
    w.bytes.resize((w.bytes.size() + 15) & ~size_t(15));
    return w.bytes;
}

size_t verifyAnimation(const SrcScene& scene, const std::vector<uint8_t>& section)
{
    if (section.empty())
        return 0;
    anim::AnimData data;
    if (!data.init(section.data(), section.size())) {
        std::fprintf(stderr, "  verify: animation section header is invalid\n");
        return 1;
    }
    std::vector<anim::Skeleton> skeletons(data.skeletonCount());
    for (uint32_t s = 0; s < skeletons.size(); ++s)
        skeletons[s].init(data, s);
    size_t violations = 0;
    for (uint32_t c = 0; c < data.clipCount(); ++c) {
        const SrcClip& src = scene.clips[c];
        const anim::Skeleton& sk = skeletons[src.skeleton];
        anim::Clip clip;
        clip.init(data, c, &sk);
        anim::Pose pose(&sk);
        const fmt::Track* tracks = data.at<fmt::Track>(data.clip(c).tracks);
        float maxAngle = 0.0f, maxTranslation = 0.0f, maxScale = 0.0f;
        bool withinRange = true;
        const uint32_t bones = sk.boneCount();
        for (uint32_t f = 0; f < src.frameCount; ++f) {
            clip.samplePose(float(f) / float(src.frameCount - 1), pose);
            for (uint32_t b = 1; b < bones; ++b) {  // bone 0 is the (identity) root
                const SrcTransform& x = src.frames[f * bones + b];
                const anim::Xform& y = pose.local(b);
                float q[4];
                normalized(x.r, q);
                const float d = std::fabs(q[0] * y.r.x + q[1] * y.r.y + q[2] * y.r.z + q[3] * y.r.w);
                maxAngle = std::max(maxAngle, 2.0f * std::acos(std::min(d, 1.0f)));
                // Quantization error is at most one 16-bit step of the track's range.
                for (int a = 0; a < 3; ++a) {
                    const float e = std::fabs((&y.t.x)[a] - x.t[a]);
                    maxTranslation = std::max(maxTranslation, e);
                    withinRange &= e <= tracks[b].translation[a].length / 65535.0f + 1e-5f;
                }
                const float es = std::fabs(y.s - uniformScale(x));
                maxScale = std::max(maxScale, es);
                withinRange &= es <= tracks[b].scale.length / 65535.0f + 1e-5f;
            }
        }
        // 15 bits over +-1/sqrt2 per quaternion component; 16 bits over each track's range.
        const bool ok = maxAngle < 1e-3f && withinRange;
        if (!ok) {
            std::fprintf(stderr, "  verify: clip '%s': max error %.2e rad, %.2e translation, %.2e scale\n",
                         src.name.c_str(), maxAngle, maxTranslation, maxScale);
            ++violations;
        }
    }
    return violations;
}

}  // namespace fx::cook
