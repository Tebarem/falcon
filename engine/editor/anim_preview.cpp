#include "editor/anim_preview.h"

#include "anim/animation_system.h"
#include "assets/scene_pak.h"
#include "core/log.h"
#include "core/profile.h"
#include "render/skinning.h"
#include "scene/scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

constexpr float kFov = 40.0f * kPi / 180.0f;

uint32_t packColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(a) << 24; }

float bitsAsFloat(uint32_t u)
{
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// p through a 3x4 row transform.
float3 transform(const float m[3][4], float3 p)
{
    return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3], m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
            m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]};
}

}  // namespace

bool AnimPreview::init(const Init& init)
{
    pak_ = init.pak;
    skinning_ = init.skinning;
    animation_ = init.animation;
    ready_ = skinning_->floatingRecordCount() > 0 &&
             renderer_.init(*init.gpu, *init.pak, *init.vt, *init.skinning);
    return ready_;
}

void AnimPreview::shutdown()
{
    close();
    renderer_.shutdown();
    ready_ = false;
}

bool AnimPreview::open(const std::string& model, std::string& error)
{
    close();
    if (!ready_) {
        error = "This scene has no skinned meshes";
        return false;
    }
    const ScenePak::Library* lib = pak_->library(model);
    if (!lib) {
        error = model + " is not in this scene yet (Reload the scene to cook it)";
        return false;
    }
    uint32_t skinCount = 0;
    const auto* skins = pak_->view<fpak::MeshSkinGpu>(fpak::Section::MeshSkins, &skinCount);
    const auto skinned = [&](uint32_t mesh) { return mesh < skinCount && skins[mesh].firstSkinVertex != fpak::kNotSkinned; };
    for (const ScenePak::LibraryNode& n : lib->nodes)
        if (skinned(n.mesh)) {
            skeleton_ = int32_t(skins[n.mesh].skeleton);
            break;
        }
    if (skeleton_ < 0) {
        error = model + " has no skinned mesh";
        return false;
    }
    std::vector<PreviewPart> parts;
    uint32_t floating = 0;
    for (const ScenePak::LibraryNode& n : lib->nodes) {
        Part p;
        p.mesh = n.mesh;
        std::memcpy(p.world, n.world, sizeof(p.world));
        if (skinned(n.mesh)) {
            if (int32_t(skins[n.mesh].skeleton) != skeleton_)
                continue;  // (another skeleton: another character)
            p.record = skinning_->bindFloating(floating++, n.mesh);
            if (p.record == ~0u) {
                error = model + " has more skinned meshes than the preview can show";
                close();
                return false;
            }
        }
        parts_.push_back(p);
    }
    model_ = model;
    pose_.reset(animation_->skeleton(uint32_t(skeleton_)));
    yaw_ = 0.0f;
    groundOffset_ = {0, 0};
    restart();
    update(0.0f, 0);  // posed and bounded, then framed
    frame();
    return true;
}

void AnimPreview::close()
{
    parts_.clear();
    graph_.reset();
    skeleton_ = -1;
    model_.clear();
    if (!ready_)
        return;
    skinning_->unbindFloating();
    renderer_.setParts({});
    renderer_.releaseTargets();
}

void AnimPreview::setGraph(std::shared_ptr<const anim::GraphDefinition> graph)
{
    if (graph && skeleton_ >= 0 && graph->skeleton != uint32_t(skeleton_))
        graph = nullptr;  // (for another skeleton: the clip plays)
    // Parameter values by name into the new instance.
    std::vector<std::pair<std::string, float>> values;
    if (graph_)
        for (uint32_t i = 0; i < graph_->parameterCount(); ++i)
            values.push_back({graph_->parameterName(i), graph_->parameterType(i) == anim::fmt::NodeType::ParamBool
                                                            ? (graph_->getBool(int32_t(i)) ? 1.0f : 0.0f)
                                                            : graph_->getFloat(int32_t(i))});
    definition_ = std::move(graph);
    restart();
    if (graph_)
        for (const auto& [name, v] : values) {
            const int32_t i = graph_->parameterIndex(name.c_str());
            if (i >= 0 && graph_->parameterType(uint32_t(i)) == anim::fmt::NodeType::ParamBool)
                graph_->setBool(i, v != 0.0f);
            else if (i >= 0 && graph_->parameterType(uint32_t(i)) == anim::fmt::NodeType::ParamFloat)
                graph_->setFloat(i, v);
        }
}

void AnimPreview::restart()
{
    graph_.reset();
    clip_ = -1;
    time_ = clipTime_ = 0.0f;
    if (skeleton_ < 0)
        return;
    const anim::Skeleton* skeleton = animation_->skeleton(uint32_t(skeleton_));
    if (definition_) {
        graph_ = std::make_unique<anim::GraphInstance>();
        if (!graph_->init(definition_, animation_->clips(), skeleton))
            graph_.reset();
    }
    for (uint32_t i = 0; i < animation_->clips().size() && !graph_ && clip_ < 0; ++i)
        if (animation_->clips()[i].skeleton() == skeleton)
            clip_ = int32_t(i);
}

void AnimPreview::pose(float dt)
{
    FX_ZONE_CAT("AnimPreview::pose", Animation);
    time_ += dt;
    anim::Xform root;
    if (graph_) {
        root = graph_->evaluate(dt);
        pose_.locals() = graph_->pose().locals();
        if (recentEvents_.size() < 512)
            recentEvents_.insert(recentEvents_.end(), graph_->events().begin(), graph_->events().end());
    } else if (clip_ >= 0) {
        const anim::Clip& clip = animation_->clips()[clip_];
        const float duration = std::max(clip.duration(), 1e-4f);
        clipTime_ = std::fmod(clipTime_ + dt, duration);
        if (clipTime_ < 0.0f)
            clipTime_ += duration;
        clip.samplePose(clipTime_ / duration, pose_);
        if (pose_.isAdditive())
            anim::applyAdditiveToReference(pose_);
    } else {
        pose_.setToReference();
    }
    pose_.calculateModelSpace();
    // Root motion moving: the character turns, and the ground slides under it (it stays at the
    // middle of the pane).
    if (options.moveRoot && dt != 0.0f) {
        const float3 fwd = anim::rotate(root.r, {0, 0, 1});
        yaw_ += std::atan2(fwd.x, fwd.z);
        const anim::Quat q = anim::fromAxisAngle({0, 1, 0}, yaw_);
        const float3 t = anim::rotate(q, root.t);
        groundOffset_ = {std::fmod(groundOffset_.x + t.x, 100.0f), std::fmod(groundOffset_.y + t.z, 100.0f)};
    } else if (!options.moveRoot) {
        yaw_ = 0.0f;
        groundOffset_ = {0, 0};
    }
}

void AnimPreview::update(float dt, uint64_t frame)
{
    if (skeleton_ < 0)
        return;
    float step = options.paused ? 0.0f : dt * options.speed;
    if (options.paused && pendingStep_ != 0.0f)
        step = pendingStep_;
    pendingStep_ = 0.0f;
    pose(step);

    // The parts in preview space (turned by root motion), skinned by this pose.
    const float c = std::cos(yaw_), s = std::sin(yaw_);
    const float turn[3][4] = {{c, 0, s, 0}, {0, 1, 0, 0}, {-s, 0, c, 0}};
    std::vector<PreviewPart> parts;
    float3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
    bool haveSkinned = false;
    float firstSkinned[3][4] = {};  // the bones' space
    for (const Part& p : parts_) {
        PreviewPart pp;
        pp.mesh = p.mesh;
        pp.skinRecord = p.record;
        affineMultiply(turn, p.world, pp.world);
        if (p.record != ~0u) {
            const float4 b = animation_->skinPose(p.record, p.mesh, pose_);
            const float3 center = transform(pp.world, {b.x, b.y, b.z});
            lo = {std::min(lo.x, center.x - b.w), std::min(lo.y, center.y - b.w), std::min(lo.z, center.z - b.w)};
            hi = {std::max(hi.x, center.x + b.w), std::max(hi.y, center.y + b.w), std::max(hi.z, center.z + b.w)};
            if (!haveSkinned) {
                std::memcpy(firstSkinned, pp.world, sizeof(firstSkinned));
                haveSkinned = true;
            }
        }
        parts.push_back(pp);
    }
    renderer_.setParts(parts);
    if (lo.x <= hi.x)
        bounds_ = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f, length(hi - lo) * 0.5f};

    // Bones: a line from each to its parent, over everything.
    lines_.clear();
    if (options.bones && haveSkinned) {
        const float w = bitsAsFloat(packColor(255, 214, 90, 255));
        const std::vector<anim::Xform>& model = pose_.model();
        const anim::Skeleton* sk = pose_.skeleton();
        for (uint32_t b = 1; b < sk->boneCount(); ++b) {
            const int32_t parent = sk->parent(b);
            if (parent < 0)
                continue;
            const float3 a = transform(firstSkinned, model[uint32_t(parent)].t), z = transform(firstSkinned, model[b].t);
            lines_.push_back({a.x, a.y, a.z, w});
            lines_.push_back({z.x, z.y, z.z, w});
        }
    }
    // Textures: the mip the character's size on screen needs.
    const float pixels = 720.0f * bounds_.w / std::max(distance_ * std::tan(kFov * 0.5f), 1e-3f);
    renderer_.requestTextures(pixels, frame);
}

void AnimPreview::frame()
{
    target_ = {bounds_.x, bounds_.y, bounds_.z};
    distance_ = std::max(bounds_.w, 0.1f) / std::tan(kFov * 0.5f) * 1.25f;
}

bool AnimPreview::handleEvent(const SDL_Event& e, const UiRect& rect, float pixelScaleX, float pixelScaleY)
{
    (void)rect;
    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        dragButtons_ |= 1u << e.button.button;
        lastMouse_ = {e.button.x, e.button.y};
        panning_ = e.button.button == SDL_BUTTON_MIDDLE || (e.button.button == SDL_BUTTON_LEFT && (SDL_GetModState() & SDL_KMOD_SHIFT));
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        dragButtons_ &= ~(1u << e.button.button);
        return true;
    case SDL_EVENT_MOUSE_MOTION: {
        // (from positions: injected events carry none of their own motion)
        const float dx = (e.motion.x - lastMouse_.x) * pixelScaleX, dy = (e.motion.y - lastMouse_.y) * pixelScaleY;
        lastMouse_ = {e.motion.x, e.motion.y};
        if (!dragButtons_)
            return true;
        if (panning_) {
            const float3 fwd = {-std::cos(orbitPitch_) * std::sin(orbitYaw_), -std::sin(orbitPitch_), -std::cos(orbitPitch_) * std::cos(orbitYaw_)};
            const float3 right = normalize(cross(fwd, {0, 1, 0}));
            const float3 up = cross(right, fwd);
            const float k = distance_ * 0.0015f;
            target_ = target_ - right * (dx * k) + up * (dy * k);
        } else {
            orbitYaw_ -= dx * 0.008f;
            orbitPitch_ = std::clamp(orbitPitch_ + dy * 0.008f, -1.45f, 1.45f);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        distance_ = std::clamp(distance_ * std::pow(0.88f, e.wheel.y), 0.05f, 500.0f);
        return true;
    case SDL_EVENT_KEY_DOWN:
        if (e.key.key == SDLK_F && !e.key.repeat) {
            frame();
            return true;
        }
        return false;
    default:
        return false;
    }
}

void AnimPreview::render(SDL_GPUCommandBuffer* cmd, const UiRect& rect)
{
    if (skeleton_ < 0 || !rect.w || !rect.h)
        return;
    // Never below the ground (it would hide the character): the lowest pitch keeping the eye over it.
    if (target_.y + std::sin(orbitPitch_) * distance_ < 0.05f)
        orbitPitch_ = std::asin(std::clamp((0.05f - target_.y) / distance_, -1.0f, 1.0f));
    const float3 back = {std::cos(orbitPitch_) * std::sin(orbitYaw_), std::sin(orbitPitch_), std::cos(orbitPitch_) * std::cos(orbitYaw_)};
    const float3 eye = target_ + back * distance_;
    const float3 fwd = back * -1.0f;
    const float3 right = normalize(cross(fwd, {0, 1, 0}));
    const float3 up = cross(right, fwd);
    const float4x4 view = viewFromBasis(right, up, fwd, eye);
    const float4x4 proj = perspectiveReverseZ(kFov, float(rect.w) / float(rect.h), std::max(distance_ * 0.01f, 0.005f));
    PreviewFrame f;
    f.viewProj = proj * view;
    f.cameraPos = eye;
    f.focus = bounds_;
    f.grid = options.grid;
    f.shadows = options.shadows;
    f.gridOffset = groundOffset_;
    f.groundRadius = std::max(30.0f, bounds_.w * 12.0f);
    f.gridCell = bounds_.w > 10.0f ? 10.0f : 1.0f;
    f.lines = &lines_;
    renderer_.render(cmd, rect.w, rect.h, f);
}

}  // namespace fx
