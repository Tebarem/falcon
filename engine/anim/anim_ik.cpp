#include "anim/anim_ik.h"
#include "core/dmath.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace fx::anim {

Quat fromTo(float3 from, float3 to)
{
    const float3 a = normalize(from), b = normalize(to);
    const float d = dot(a, b);
    if (d < -0.99999f) {  // opposite: half a turn about any axis across them
        float3 axis = cross(float3{1.0f, 0.0f, 0.0f}, a);
        if (length(axis) < 1e-4f)
            axis = cross(float3{0.0f, 1.0f, 0.0f}, a);
        return fromAxisAngle(axis, 3.14159265f);
    }
    const float3 c = cross(a, b);
    return normalize(Quat{c.x, c.y, c.z, 1.0f + d});
}

namespace {

void setLocalRotation(Pose& pose, uint32_t bone, Quat rotation)
{
    const int32_t parent = pose.skeleton()->parent(bone);
    const Quat parentRotation = parent >= 0 ? pose.model()[uint32_t(parent)].r : Quat{};
    pose.local(bone).r = normalize(mul(conjugate(parentRotation), rotation));
}

// A bone's model rotation set, model space brought up to date along the path to `to` only (the
// solves read nothing else until they finish).
void setModelRotationTo(Pose& pose, uint32_t bone, Quat rotation, uint32_t to)
{
    setLocalRotation(pose, bone, rotation);
    pose.calculateModelPath(bone, to);
}

// Blends bones' locals from `before` (their values before a solve) to what the solve left, and
// brings model space up to date below the first (the chain's root: parents come first).
void blendFrom(Pose& pose, const std::vector<uint32_t>& bones, const std::vector<Xform>& before, float weight)
{
    if (bones.empty())
        return;
    if (weight < 1.0f)
        for (size_t i = 0; i < bones.size(); ++i)
            pose.local(bones[i]) = lerp(before[i], pose.local(bones[i]), std::max(weight, 0.0f));
    pose.calculateModelSpace(*std::min_element(bones.begin(), bones.end()));
}

}  // namespace

void setModelRotation(Pose& pose, uint32_t bone, Quat rotation)
{
    setLocalRotation(pose, bone, rotation);
    pose.calculateModelSpace(bone);
}

void solveTwoBone(Pose& pose, uint32_t upper, uint32_t mid, uint32_t end, float3 target, float weight, bool keepEnd)
{
    if (weight <= 0.0f)
        return;
    const std::vector<uint32_t> bones = {upper, mid, end};
    const std::vector<Xform> before = {pose.local(upper), pose.local(mid), pose.local(end)};
    const Quat endRotation = pose.model()[end].r;
    const float3 a = pose.model()[upper].t, b = pose.model()[mid].t, c = pose.model()[end].t;
    const float upperLength = length(b - a), lowerLength = length(c - b);
    if (upperLength < 1e-6f || lowerLength < 1e-6f)
        return;

    // The plane it bends in: now, else at rest (held straight), else any.
    float3 normal = cross(c - a, b - a);
    if (length(normal) < 1e-6f * upperLength * lowerLength) {
        const Skeleton& s = *pose.skeleton();
        normal = cross(s.referenceModel(end).t - s.referenceModel(upper).t, s.referenceModel(mid).t - s.referenceModel(upper).t);
    }
    if (length(normal) < 1e-9f)
        normal = cross(c - a, std::fabs(c.y - a.y) > std::fabs(c.x - a.x) ? float3{1.0f, 0.0f, 0.0f} : float3{0.0f, 1.0f, 0.0f});
    normal = normalize(normal);

    // Where the middle joint goes: the triangle of the two bones and the reach (law of cosines),
    // on the side it bends to now.
    float3 toTarget = target - a;
    const float reach = std::clamp(length(toTarget), std::fabs(upperLength - lowerLength) + 1e-5f, upperLength + lowerLength - 1e-5f);
    toTarget = length(toTarget) > 1e-6f ? normalize(toTarget) : normalize(c - a);
    const float cosAngle = std::clamp((upperLength * upperLength + reach * reach - lowerLength * lowerLength) / (2.0f * upperLength * reach), -1.0f, 1.0f);
    float3 side = normalize(cross(normal, toTarget));
    if (dot(side, b - a) < 0.0f)
        side = side * -1.0f;
    const float3 midTarget = a + toTarget * (upperLength * cosAngle) + side * (upperLength * std::sqrt(1.0f - cosAngle * cosAngle));

    setModelRotationTo(pose, upper, mul(fromTo(b - a, midTarget - a), pose.model()[upper].r), end);
    const float3 b1 = pose.model()[mid].t, c1 = pose.model()[end].t;
    setModelRotationTo(pose, mid, mul(fromTo(c1 - b1, a + toTarget * reach - b1), pose.model()[mid].r), end);
    if (keepEnd)
        setModelRotationTo(pose, end, endRotation, end);
    blendFrom(pose, bones, before, weight);
}

void solveLook(Pose& pose, const std::vector<uint32_t>& chain, const std::vector<float>& shares, float3 forward,
               float3 target, float maxAngle, float weight)
{
    if (!chain.empty())
        solveAim(pose, chain, shares, chain.back(), forward, target, maxAngle, weight, 1);
}

void solveAim(Pose& pose, const std::vector<uint32_t>& chain, const std::vector<float>& shares, uint32_t effector,
              float3 forward, float3 target, float maxAngle, float weight, int passes)
{
    if (chain.empty() || weight <= 0.0f)
        return;
    std::vector<Xform> before;
    for (const uint32_t b : chain)
        before.push_back(pose.local(b));

    // Where to point: from the effector at the target, at most maxAngle from where it points now.
    const float3 start = normalize(rotate(pose.model()[effector].r, forward));
    const auto wanted = [&](float3& want) {
        want = target - pose.model()[effector].t;
        if (length(want) < 1e-6f)
            return false;
        want = normalize(want);
        if (dmath::acos(std::clamp(dot(start, want), -1.0f, 1.0f)) > maxAngle) {
            float3 axis = cross(start, want);
            if (length(axis) < 1e-6f)
                axis = cross(start, float3{0.0f, 1.0f, 0.0f});
            want = rotate(fromAxisAngle(axis, maxAngle), start);
        }
        return true;
    };

    // Each bone, root first, takes its share of what is left of the turn; the last all of the rest.
    // An effector off the chain moves as it turns (the direction to the target with it): passes
    // again from there.
    for (int pass = 0; pass < std::max(passes, 1); ++pass) {
        float left = 0.0f;
        for (size_t i = 0; i < chain.size(); ++i)
            left += i < shares.size() ? std::max(shares[i], 0.0f) : 1.0f;
        for (size_t i = 0; i < chain.size(); ++i) {
            const float share = i < shares.size() ? std::max(shares[i], 0.0f) : 1.0f;
            const float part = i + 1 == chain.size() || left <= 0.0f ? 1.0f : share / left;
            left -= share;
            float3 want;
            if (!wanted(want))
                break;
            if (part <= 0.0f)
                continue;  // (no share of the turn)
            const float3 pointing = rotate(pose.model()[effector].r, forward);
            const Quat turn = slerp(Quat{}, fromTo(pointing, want), part);
            setModelRotationTo(pose, chain[i], mul(turn, pose.model()[chain[i]].r), effector);
        }
    }
    blendFrom(pose, chain, before, weight);
}

void solveHold(Pose& pose, uint32_t bone, float3 forward, float3 at, float3 target, int32_t support, float weight)
{
    const Skeleton& s = *pose.skeleton();
    const int32_t mid = s.parent(bone), upper = mid >= 0 ? s.parent(uint32_t(mid)) : -1;
    if (upper < 0 || weight <= 0.0f)
        return;

    // The other hand's grip, as the pose holds it (in the bone's space).
    const int32_t supportMid = support >= 0 ? s.parent(uint32_t(support)) : -1;
    const int32_t supportUpper = supportMid >= 0 ? s.parent(uint32_t(supportMid)) : -1;
    float3 gripAt = {};
    Quat gripTurn;
    if (supportUpper >= 0) {
        const Xform& a = pose.model()[bone];
        const Xform& b = pose.model()[uint32_t(support)];
        gripAt = inverseRotate(a.r, b.t - a.t);
        gripTurn = mul(conjugate(a.r), b.r);
    }

    // The hand to its place, then pointing at the target from there.
    solveTwoBone(pose, uint32_t(upper), uint32_t(mid), bone, at, weight, true);
    const float3 want = target - pose.model()[bone].t;
    if (length(want) > 1e-6f) {
        const float3 pointing = rotate(pose.model()[bone].r, forward);
        setModelRotation(pose, bone, mul(slerp(Quat{}, fromTo(pointing, want), weight), pose.model()[bone].r));
    }

    // The other hand where its grip is now.
    if (supportUpper >= 0) {
        const Xform& a = pose.model()[bone];
        const float3 gripNow = a.t + rotate(a.r, gripAt);
        const Quat turnNow = mul(a.r, gripTurn);
        solveTwoBone(pose, uint32_t(supportUpper), uint32_t(supportMid), uint32_t(support), gripNow, 1.0f, false);
        setModelRotation(pose, uint32_t(support), turnNow);
    }
}

std::vector<uint32_t> lookChain(const Skeleton& skeleton, uint32_t tip)
{
    const auto looks = [&](uint32_t bone) {
        std::string n = skeleton.name(bone);
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        for (const char* part : {"spine", "neck", "head", "chest"})
            if (n.find(part) != std::string::npos)
                return true;
        return false;
    };
    std::vector<uint32_t> chain = {tip};
    for (int32_t b = skeleton.parent(tip); b >= 0 && looks(uint32_t(b)); b = skeleton.parent(uint32_t(b)))
        chain.push_back(uint32_t(b));
    std::reverse(chain.begin(), chain.end());
    return chain;
}

std::vector<uint32_t> aimChain(const Skeleton& skeleton, uint32_t bone)
{
    const auto spine = [&](uint32_t b) {
        std::string n = skeleton.name(b);
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        return n.find("spine") != std::string::npos || n.find("chest") != std::string::npos;
    };
    std::vector<uint32_t> chain;
    for (int32_t b = skeleton.parent(bone); b >= 0; b = skeleton.parent(uint32_t(b))) {
        if (spine(uint32_t(b)))
            chain.push_back(uint32_t(b));
        else if (!chain.empty())
            break;  // below the spine (the pelvis)
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

}  // namespace fx::anim
