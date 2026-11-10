#include "anim/animation_system.h"

#include "anim/anim_graph.h"
#include "anim/anim_ik.h"

#include "assets/scene_pak.h"
#include "core/dmath.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/profile.h"
#include "render/skinning.h"
#include "scene/scene.h"

#include <SDL3/SDL_timer.h>
#include <TaskScheduler.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fx {

namespace {

// a (3x4 rows) x b (3x4 rows), both affine.
void mul3x4(const float* a, const float* b, float* out)
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] + a[r * 4 + 2] * b[2 * 4 + c];
            if (c == 3)
                v += a[r * 4 + 3];
            out[r * 4 + c] = v;
        }
    }
}

}  // namespace

bool AnimationSystem::init(const anim::AnimData& data, const ScenePak& pak, SkinningSystem& skinning)
{
    FX_ZONE_CAT("AnimationSystem::init", Animation);
    data_ = &data;
    pak_ = &pak;
    skinning_ = &skinning;
    skeletons_.resize(data.skeletonCount());
    for (uint32_t s = 0; s < data.skeletonCount(); ++s)
        skeletons_[s].init(data, s);
    clips_.resize(data.clipCount());
    for (uint32_t c = 0; c < data.clipCount(); ++c)
        clips_[c].init(data, c, &skeletons_[data.clip(c).skeleton]);

    // Per skinned mesh: how far its weighted vertices reach from each bone (rest pose), so the
    // animated bounds are the union of spheres around the posed bones.
    uint32_t meshCount = 0;
    const auto* meshes = pak.view<fpak::MeshGpu>(fpak::Section::Meshes, &meshCount);
    const auto* skins = pak.view<fpak::MeshSkinGpu>(fpak::Section::MeshSkins);
    const auto* vertices = pak.view<fpak::VertexGpu>(fpak::Section::Vertices);
    const auto* skinVertices = pak.view<fpak::SkinVertexGpu>(fpak::Section::SkinVertices);
    meshBoneRadii_.resize(meshCount);
    for (uint32_t m = 0; m < meshCount; ++m) {
        const fpak::MeshSkinGpu& s = skins[m];
        if (s.firstSkinVertex == fpak::kNotSkinned || s.skeleton >= skeletons_.size())
            continue;
        const anim::Skeleton& sk = skeletons_[s.skeleton];
        std::vector<float>& radii = meshBoneRadii_[m];
        radii.assign(sk.boneCount(), 0.0f);
        for (uint32_t v = 0; v < s.vertexCount; ++v) {
            const fpak::VertexGpu& g = vertices[s.firstVertex + v];
            const float3 p = {meshes[m].posOffset[0] + float(g.posXY & 0xffff) * meshes[m].posScale[0],
                              meshes[m].posOffset[1] + float(g.posXY >> 16) * meshes[m].posScale[1],
                              meshes[m].posOffset[2] + float(g.posZ & 0xffff) * meshes[m].posScale[2]};
            const fpak::SkinVertexGpu& w = skinVertices[s.firstSkinVertex + v];
            for (int k = 0; k < 4; ++k)
                if (w.weights[k] > 0 && w.joints[k] < radii.size())
                    radii[w.joints[k]] = std::max(radii[w.joints[k]], length(p - sk.referenceModel(w.joints[k]).t));
        }
    }

    // One character per skinned instance (indexed by its record).
    for (uint32_t r = 0; r < skinning.instanceRecordCount(); ++r) {
        Character c;
        c.record = r;
        c.skeleton = &skeletons_[skinning.skeletonOfRecord(r)];
        c.pose.reset(c.skeleton);
        c.boneRadii = &meshBoneRadii_[pak.instances()[skinning.instanceOfRecord(r)].mesh];
        start(c);
        characters_.push_back(std::move(c));
    }
    if (!characters_.empty())
        FX_INFO("animation: %u skeletons, %u clips, %u graphs, %zu characters", data.skeletonCount(), data.clipCount(),
                data.graphCount(), characters_.size());
    return true;
}

void AnimationSystem::shutdown()
{
    characters_.clear();
}

void AnimationSystem::start(Character& c)
{
    c.graph.reset();
    c.clip = -1;
    c.time = 0.0f;
    c.played = 0.0f;
    c.rootMotion = {};
    // An Animator's graph; else the skeleton's first cooked graph; else its first clip, looping.
    std::shared_ptr<const anim::GraphDefinition> def = c.definition;
    for (uint32_t g = 0; g < data_->graphCount() && !def; ++g)
        if (data_->graph(g).skeleton == c.skeleton->index())
            def = anim::GraphDefinition::fromPak(*data_, g);
    if (def) {
        c.graph = std::make_unique<anim::GraphInstance>();
        if (!c.graph->init(def, clips_, c.skeleton)) {
            FX_WARN("animation: graph '%s' could not be instantiated", def->name.c_str());
            c.graph.reset();
        }
    }
    useIk(c);
    if (c.graph) {
        for (const auto& [name, value] : c.params) {
            const int32_t i = c.graph->parameterIndex(name.c_str());
            if (i >= 0 && c.graph->parameterType(uint32_t(i)) == anim::fmt::NodeType::ParamBool)
                c.graph->setBool(i, value != 0.0f);
            else
                c.graph->setFloat(i, value);
        }
        return;
    }
    for (uint32_t i = 0; i < clips_.size() && c.clip < 0; ++i)
        if (clips_[i].skeleton() == c.skeleton)
            c.clip = int32_t(i);
}

void AnimationSystem::useIk(Character& c)
{
    // The IK it solves: its graph's (the instance keeps the definition alive), else its Animator's
    // own; what the last ones drove (and no script took) stops.
    const anim::IkSettings* ik = c.graph && c.graph->definition().ik.any() ? &c.graph->definition().ik : c.ownIk.get();
    if (c.ik.settings == ik)
        return;
    if (!c.ik.lookScripted)
        c.ik.lookWeight = 0.0f;
    if (!c.ik.aimScripted)
        c.ik.aimWeight = 0.0f;
    std::erase_if(c.ik.reaches, [&](const Character::Reach& r) {
        return std::find(c.ik.scriptedReaches.begin(), c.ik.scriptedReaches.end(), r.bone) == c.ik.scriptedReaches.end();
    });
    c.ik.settings = ik;
}

void AnimationSystem::detach(uint32_t record)
{
    Character& c = characters_[record];
    if (c.leader >= 0) {
        std::vector<uint32_t>& f = characters_[c.leader].followers;
        f.erase(std::remove(f.begin(), f.end(), record), f.end());
        c.leader = -1;
    }
    for (const uint32_t r : c.followers) {
        characters_[r].leader = -1;
        start(characters_[r]);
    }
    c.followers.clear();
    c.definition = nullptr;
    c.params.clear();
    c.ownIk = nullptr;
    c.entities.clear();
    c.preview = false;
    start(c);
}

uint32_t AnimationSystem::bind(const std::vector<uint32_t>& records, std::shared_ptr<const anim::GraphDefinition> graph,
                               const Params& params)
{
    for (const uint32_t r : records)
        if (r < characters_.size())
            detach(r);
    if (records.empty() || records[0] >= characters_.size() || !graph)
        return 0;
    Character& lead = characters_[records[0]];
    if (graph->skeleton != lead.skeleton->index()) {
        FX_WARN("animation: graph '%s' is for another skeleton than '%s'", graph->name.c_str(),
                data_->name(data_->skeleton(lead.skeleton->index()).name));
        return 0;
    }
    lead.definition = std::move(graph);
    lead.params = params;
    start(lead);
    uint32_t bound = 1;
    for (size_t i = 1; i < records.size(); ++i) {
        if (records[i] >= characters_.size())
            continue;
        Character& f = characters_[records[i]];
        if (records[i] == lead.record || f.skeleton != lead.skeleton || f.leader >= 0)
            continue;
        f.graph.reset();
        f.clip = -1;
        f.leader = int32_t(lead.record);
        lead.followers.push_back(records[i]);
        ++bound;
    }
    return bound;
}

std::vector<uint32_t> AnimationSystem::recordsUnder(flecs::world& world, flecs::entity_t root) const
{
    std::vector<uint32_t> out;
    if (!root || !world.is_alive(root))
        return out;
    const auto recordOf = [&](flecs::entity e) {
        const MeshInstance* mi = e.try_get<MeshInstance>();
        return mi ? skinning_->recordOfInstance(mi->gpuSlot) : ~0u;
    };
    if (const uint32_t r = recordOf(world.entity(root)); r != ~0u)
        out.push_back(r);
    world.query_builder<const MeshInstance>().query_flags(EcsQueryMatchDisabled).build().each([&](flecs::entity e, const MeshInstance&) {
        const uint32_t r = recordOf(e);
        if (r == ~0u || e.id() == root)
            return;
        for (const Parent* p = e.try_get<Parent>(); p; p = world.entity(p->entity).try_get<Parent>())
            if (p->entity == root) {
                out.push_back(r);
                return;
            }
    });
    return out;
}

void AnimationSystem::reset()
{
    for (Character& c : characters_) {
        c.hidden.clear();
        c.collapse.clear();
        c.rate = 1.0f;
        c.play.clip = -1;  // (scripts' clips and speeds gone)
        // IK back to the graph's (scripts' looks, aims, reaches, weights and entities gone).
        c.ik.feetWeight = 0.0f;
        c.ik.feetScripted = false;
        c.ik.settled = false;
        c.ik.footOffset[0] = c.ik.footOffset[1] = c.ik.pelvisOffset = 0.0f;
        c.ik.lookWeight = 0.0f;
        c.ik.aimWeight = 0.0f;
        c.ik.reaches.clear();
        c.ik.lookScripted = c.ik.aimScripted = false;  // (the graph's IK followed again)
        c.ik.scriptedReaches.clear();
        std::erase_if(c.entities, [](const Character::EntitySlot& s) { return !s.bound; });
        for (Character::EntitySlot& s : c.entities)
            s.isScripted = false, s.scripted = 0;
        if (c.leader < 0)
            start(c);
    }
}

bool AnimationSystem::hideBone(uint32_t record, std::string_view bone, bool hidden)
{
    if (record >= characters_.size())
        return false;
    Character& c = characters_[characters_[record].leader >= 0 ? uint32_t(characters_[record].leader) : record];
    const int32_t b = c.skeleton->findBone(std::string(bone).c_str());
    if (b < 0)
        return false;
    const auto at = std::find(c.hidden.begin(), c.hidden.end(), b);
    if (hidden == (at != c.hidden.end()))
        return true;
    if (hidden)
        c.hidden.push_back(b);
    else
        c.hidden.erase(at);
    // Each bone collapses into its highest hidden ancestor (parents come before their children).
    c.collapse.clear();
    if (c.hidden.empty())
        return true;
    c.collapse.assign(c.skeleton->boneCount(), -1);
    for (uint32_t i = 0; i < c.skeleton->boneCount(); ++i) {
        const int32_t parent = c.skeleton->parent(i);
        if (parent >= 0 && c.collapse[uint32_t(parent)] >= 0)
            c.collapse[i] = c.collapse[uint32_t(parent)];
        else if (std::find(c.hidden.begin(), c.hidden.end(), int32_t(i)) != c.hidden.end())
            c.collapse[i] = int32_t(i);
    }
    return true;
}

bool AnimationSystem::boneHidden(uint32_t record, std::string_view bone) const
{
    if (record >= characters_.size())
        return false;
    const Character& c = characters_[characters_[record].leader >= 0 ? uint32_t(characters_[record].leader) : record];
    const int32_t b = c.skeleton->findBone(std::string(bone).c_str());
    return b >= 0 && !c.collapse.empty() && c.collapse[uint32_t(b)] >= 0;
}

uint32_t AnimationSystem::replaceGraph(const anim::GraphDefinition* old, std::shared_ptr<const anim::GraphDefinition> now)
{
    uint32_t replaced = 0;
    for (Character& c : characters_) {
        if (!old || c.definition.get() != old || c.leader >= 0)
            continue;
        std::vector<std::pair<std::string, float>> values;
        if (c.graph)
            for (uint32_t i = 0; i < c.graph->parameterCount(); ++i)
                values.push_back({c.graph->parameterName(i), c.graph->parameterType(i) == anim::fmt::NodeType::ParamBool
                                                                 ? (c.graph->getBool(int32_t(i)) ? 1.0f : 0.0f)
                                                                 : c.graph->getFloat(int32_t(i))});
        c.definition = now;
        start(c);
        for (const auto& [name, v] : values)
            if (const int32_t i = c.graph ? c.graph->parameterIndex(name.c_str()) : -1; i >= 0) {
                if (c.graph->parameterType(uint32_t(i)) == anim::fmt::NodeType::ParamBool)
                    c.graph->setBool(i, v != 0.0f);
                else if (c.graph->parameterType(uint32_t(i)) == anim::fmt::NodeType::ParamFloat)
                    c.graph->setFloat(i, v);
            }
        ++replaced;
    }
    return replaced;
}

int32_t AnimationSystem::modelSkeleton(const std::string& model) const
{
    const ScenePak::Library* lib = pak_ ? pak_->library(model) : nullptr;
    uint32_t count = 0;
    const auto* skins = pak_ ? pak_->view<fpak::MeshSkinGpu>(fpak::Section::MeshSkins, &count) : nullptr;
    for (const ScenePak::LibraryNode& n : lib ? lib->nodes : std::vector<ScenePak::LibraryNode>{})
        if (n.mesh < count && skins[n.mesh].firstSkinVertex != fpak::kNotSkinned)
            return int32_t(skins[n.mesh].skeleton);
    return -1;
}

uint32_t AnimationSystem::applyMarkup(const anim::Markup& markup, uint32_t skeleton, bool replace)
{
    uint32_t applied = 0;
    for (anim::Clip& c : clips_) {
        if (skeleton != ~0u && c.skeleton()->index() != skeleton)
            continue;
        // By its name, or (a model's clip renamed "<model>#<clip>" beside another of that name) the clip's own.
        std::string name = c.name();
        auto it = markup.find(name);
        if (it == markup.end() && name.find('#') != std::string::npos)
            it = markup.find(name.substr(name.rfind('#') + 1));
        if (it == markup.end()) {
            if (replace && (!c.syncEvents().empty() || !c.events().empty()))
                c.setMarkup({}, {});  // (taken out of the file)
            continue;
        }
        c.setMarkup(it->second.sync, it->second.events);
        ++applied;
    }
    for (Character& ch : characters_)
        if (ch.leader < 0)
            start(ch);
    return applied;
}

void AnimationSystem::loadProjectMarkup(const std::string& projectDir)
{
    if (!pak_ || projectDir.empty())
        return;
    for (const ScenePak::Library& lib : pak_->libraries()) {
        const int32_t skeleton = modelSkeleton(lib.source);
        if (skeleton < 0)
            continue;
        std::ifstream in(std::filesystem::path(projectDir) / anim::markupPathFor(lib.source), std::ios::binary);
        if (!in)
            continue;
        std::stringstream text;
        text << in.rdbuf();
        anim::Markup markup;
        std::string error;
        if (!anim::parseMarkup(text.str(), markup, error)) {
            FX_WARN("animation: %s: %s", anim::markupPathFor(lib.source).c_str(), error.c_str());
            continue;
        }
        markupText_[lib.source] = text.str();
        applyMarkup(markup, uint32_t(skeleton));
    }
}

const std::vector<anim::SampledEvent>* AnimationSystem::events(uint32_t record) const
{
    if (record >= characters_.size())
        return nullptr;
    const Character& c = characters_[characters_[record].leader >= 0 ? uint32_t(characters_[record].leader) : record];
    return c.graph ? &c.graph->events() : nullptr;
}

const anim::Pose* AnimationSystem::pose(uint32_t record) const
{
    if (record >= characters_.size())
        return nullptr;
    const Character& c = characters_[characters_[record].leader >= 0 ? uint32_t(characters_[record].leader) : record];
    return c.pose.boneCount() ? &c.pose : nullptr;
}

void AnimationSystem::playClip(uint32_t record, int32_t clip, float speed)
{
    if (record >= characters_.size())
        return;
    detach(record);
    Character& c = characters_[record];
    c.clip = clip >= 0 && uint32_t(clip) < clips_.size() && clips_[clip].skeleton() == c.skeleton ? clip : -1;
    c.graph.reset();
    c.speed = speed;
    c.time = 0.0f;
}

anim::GraphInstance* AnimationSystem::graph(uint32_t record)
{
    if (record >= characters_.size())
        return nullptr;
    const Character& c = characters_[record];
    return c.leader >= 0 ? characters_[c.leader].graph.get() : c.graph.get();
}

anim::Xform AnimationSystem::rootMotion(uint32_t record) const
{
    if (record >= characters_.size())
        return {};
    const Character& c = characters_[record];
    return c.leader >= 0 ? characters_[c.leader].rootMotion : c.rootMotion;
}

bool AnimationSystem::play(uint32_t record, std::string_view clip, float speed, float fade, bool loop)
{
    if (record >= characters_.size())
        return false;
    Character& c = leaderOf(record);
    Character::Play& p = c.play;
    if (clip.empty()) {
        p.stopping = true;
        p.fade = std::max(fade, 0.0f);
        return true;
    }
    for (uint32_t i = 0; i < clips_.size(); ++i)
        if (clips_[i].skeleton() == c.skeleton && clip == clips_[i].name()) {
            if (p.clip < 0)
                p.weight = 0.0f;  // (fading in from the graph; another clip: from where it is)
            p.clip = int32_t(i);
            p.speed = speed;
            p.time = speed < 0.0f ? clips_[i].duration() : 0.0f;
            p.fade = std::max(fade, 0.0f);
            p.loop = loop;
            p.stopping = false;
            return true;
        }
    return false;
}

float AnimationSystem::playing(uint32_t record) const
{
    if (record >= characters_.size())
        return -1.0f;
    const Character::Play& p = leaderOf(record).play;
    return p.clip >= 0 && !p.stopping ? p.time : -1.0f;
}

void AnimationSystem::setRate(uint32_t record, float rate)
{
    if (record < characters_.size())
        leaderOf(record).rate = std::max(rate, 0.0f);
}

float AnimationSystem::rate(uint32_t record) const
{
    return record < characters_.size() ? leaderOf(record).rate : 1.0f;
}

void AnimationSystem::overlay(Character& c, float dt)
{
    Character::Play& p = c.play;
    const anim::Clip& clip = clips_[p.clip];
    const float duration = std::max(clip.duration(), 1e-4f);
    p.time += dt * p.speed;
    if (p.loop) {
        p.time = std::fmod(p.time, duration);
        if (p.time < 0.0f)
            p.time += duration;
    } else if (p.time >= duration || p.time < 0.0f) {
        p.time = std::clamp(p.time, 0.0f, duration);
        p.stopping = true;  // (played once: fading out)
    }
    const float step = p.fade > 0.0f ? dt / p.fade : 1.0f;
    p.weight = p.stopping ? std::max(p.weight - step, 0.0f) : std::min(p.weight + step, 1.0f);
    if (p.stopping && p.weight <= 0.0f) {
        p.clip = -1;
        return;
    }
    if (p.pose.skeleton() != c.skeleton)
        p.pose.reset(c.skeleton);
    clip.samplePose(p.time / duration, p.pose);
    if (p.pose.isAdditive())
        anim::applyAdditiveToReference(p.pose);
    anim::blendPoses(c.pose, p.pose, p.weight, anim::BlendMode::Normal, nullptr, c.pose);
}

void AnimationSystem::evaluate(Character& c, float dt)
{
    dt *= c.rate;
    c.played += dt;
    if (c.graph) {
        c.rootMotion = c.graph->evaluate(dt);
        c.pose.locals() = c.graph->pose().locals();
    } else if (c.clip >= 0) {
        const anim::Clip& clip = clips_[c.clip];
        const float duration = std::max(clip.duration(), 1e-4f);
        c.time = std::fmod(c.time + dt * c.speed, duration);
        if (c.time < 0.0f)
            c.time += duration;
        clip.samplePose(c.time / duration, c.pose);
        if (c.pose.isAdditive())
            anim::applyAdditiveToReference(c.pose);
    } else {
        c.pose.setToReference();
    }
    if (c.play.clip >= 0)
        overlay(c, dt);
    c.pose.calculateModelSpace();
    if (c.ik.active())
        return;  // (applyIk skins it)
    writeSkins(c);
}

namespace {

float3 toWorld(const float w[3][4], float3 p)
{
    return {w[0][0] * p.x + w[0][1] * p.y + w[0][2] * p.z + w[0][3], w[1][0] * p.x + w[1][1] * p.y + w[1][2] * p.z + w[1][3],
            w[2][0] * p.x + w[2][1] * p.y + w[2][2] * p.z + w[2][3]};
}

float3 directionToModel(const float inverse[3][4], float3 d)
{
    return normalize(float3{inverse[0][0] * d.x + inverse[0][1] * d.y + inverse[0][2] * d.z,
                            inverse[1][0] * d.x + inverse[1][1] * d.y + inverse[1][2] * d.z,
                            inverse[2][0] * d.x + inverse[2][1] * d.y + inverse[2][2] * d.z});
}

// A bone of one side by name: "foot_l", "LeftFoot", "mixamorig:LeftFoot", "Foot.L"; not UE's IK
// helper bones ("ik_foot_l") or the toes.
int32_t sideBone(const anim::Skeleton& s, const char* part, bool left)
{
    for (uint32_t b = 0; b < s.boneCount(); ++b) {
        std::string n = s.name(b);
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        if (n.find(part) == std::string::npos || n.find("ik") != std::string::npos || n.find("toe") != std::string::npos ||
            n.find("ball") != std::string::npos || n.find("twist") != std::string::npos)
            continue;
        const char* word = left ? "left" : "right";
        const char side = left ? 'l' : 'r';
        const bool marked = n.find(word) != std::string::npos ||
                            (n.size() >= 2 && n[n.size() - 1] == side && (n[n.size() - 2] == '_' || n[n.size() - 2] == '.')) ||
                            (n.size() >= 2 && n[0] == side && n[1] == '_');
        if (marked)
            return int32_t(b);
    }
    return -1;
}

}  // namespace

bool AnimationSystem::findLegs(Character& c)
{
    Character::Ik& k = c.ik;
    if (k.pelvis == -2)
        return false;
    if (k.pelvis >= 0)
        return true;
    for (int side = 0; side < 2; ++side) {
        k.foot[side] = sideBone(*c.skeleton, "foot", side == 0);
        k.calf[side] = k.foot[side] >= 0 ? c.skeleton->parent(uint32_t(k.foot[side])) : -1;
        k.thigh[side] = k.calf[side] >= 0 ? c.skeleton->parent(uint32_t(k.calf[side])) : -1;
    }
    const int32_t pelvis = k.thigh[0] >= 0 ? c.skeleton->parent(uint32_t(k.thigh[0])) : -1;
    if (k.thigh[1] < 0 || pelvis < 0 || c.skeleton->parent(uint32_t(k.thigh[1])) != pelvis) {
        k.pelvis = -2;
        FX_WARN("animation: foot IK finds no legs in skeleton %u (foot_l / LeftFoot, its calf and thigh)", c.skeleton->index());
        return false;
    }
    k.pelvis = pelvis;
    return true;
}

const AnimationSystem::Character::EntitySlot* AnimationSystem::slot(const Character& c, std::string_view name) const
{
    for (const Character::EntitySlot& s : c.entities)
        if (s.name == name)
            return &s;
    return nullptr;
}

void AnimationSystem::followIk(Character& c)
{
    Character::Ik& k = c.ik;
    const anim::IkSettings* s = k.settings;
    // A weight: a constant, or a parameter of the graph (bools 0 / 1).
    const auto weightOf = [&](const anim::IkWeight& w) {
        if (w.parameter.empty())
            return w.value;
        const int32_t i = c.graph ? c.graph->parameterIndex(w.parameter.c_str()) : -1;
        if (i < 0)
            return 0.0f;
        return c.graph->parameterType(uint32_t(i)) == anim::fmt::NodeType::ParamBool ? (c.graph->getBool(i) ? 1.0f : 0.0f)
                                                                                     : std::clamp(c.graph->getFloat(i), 0.0f, 1.0f);
    };
    if (!k.feetScripted)
        k.feetWeight = s && s->feet.on ? weightOf(s->feet.weight) : 0.0f;
    k.feetReach = s && s->feet.on ? s->feet.reach : 0.45f;
    if (!s)
        return;
    followingTargets_ = true;
    float3 p;
    float world[3][4];
    const auto at = [&](const std::string& name) {
        const Character::EntitySlot* e = name.empty() ? nullptr : slot(c, name);
        if (!e || !e->value() || !entityAt_ || !entityAt_(e->value(), world))
            return false;
        p = {world[0][3], world[1][3], world[2][3]};
        return true;
    };
    if (s->look.on && !k.lookScripted) {
        const bool found = at(s->look.target);
        lookAt(c.record, found ? p : float3{}, found ? weightOf(s->look.weight) : 0.0f, s->look.bone);
    }
    if (s->aim.on && !k.aimScripted) {
        // Held: at the hold, pointing at the aim target, else along the hold's forward (-Z).
        float3 hold = {}, target = {};
        const bool held = at(s->aim.hold);
        if (held) {
            hold = p;
            const float3 forward = float3{-world[0][2], -world[1][2], -world[2][2]};
            target = hold + (length(forward) > 1e-6f ? normalize(forward) : float3{0.0f, 0.0f, -1.0f}) * 20.0f;
        }
        const bool aimed = at(s->aim.target);
        if (aimed)
            target = p;
        // The other hand: the aim bone's other side (unless one is named, or "none").
        std::string support = s->aim.support == "none" ? std::string() : s->aim.support;
        if (s->aim.support.empty()) {
            const int32_t bone = c.skeleton->findBone(s->aim.bone.c_str());
            const int32_t left = sideBone(*c.skeleton, "hand", true), right = sideBone(*c.skeleton, "hand", false);
            const int32_t other = bone >= 0 && bone == left ? right : bone >= 0 && bone == right ? left : -1;
            if (other >= 0)
                support = c.skeleton->name(uint32_t(other));
        }
        aimAt(c.record, target, held || aimed ? weightOf(s->aim.weight) : 0.0f, s->aim.bone, s->aim.axis, held ? &hold : nullptr, support);
        k.aimSpine = s->aim.spine;
    }
    for (const anim::IkSettings::Reach& r : s->reaches) {
        int32_t hand = c.skeleton->findBone(r.bone.c_str());
        if (hand < 0 && (r.bone == "left hand" || r.bone == "right hand"))
            hand = sideBone(*c.skeleton, "hand", r.bone[0] == 'l');
        if (hand < 0 || std::find(k.scriptedReaches.begin(), k.scriptedReaches.end(), uint32_t(hand)) != k.scriptedReaches.end())
            continue;
        const bool found = at(r.target);
        reach(c.record, c.skeleton->name(uint32_t(hand)), found ? p : float3{}, found ? weightOf(r.weight) : 0.0f);
    }
    followingTargets_ = false;
}

void AnimationSystem::setOwnIk(uint32_t record, std::shared_ptr<const anim::IkSettings> ik)
{
    if (record >= characters_.size())
        return;
    Character& c = leaderOf(record);
    c.ownIk = ik && ik->any() ? std::move(ik) : nullptr;
    c.ik.settled = false;
    useIk(c);
}

const anim::IkSettings* AnimationSystem::ikSettings(uint32_t record) const
{
    return record < characters_.size() ? leaderOf(record).ik.settings : nullptr;
}

void AnimationSystem::bindEntities(uint32_t record, const std::vector<std::pair<std::string, uint64_t>>& entities)
{
    if (record >= characters_.size())
        return;
    Character& c = leaderOf(record);
    std::erase_if(c.entities, [](const Character::EntitySlot& s) { return !s.isScripted; });
    for (Character::EntitySlot& s : c.entities)
        s.bound = 0;
    for (const auto& [name, entity] : entities) {
        if (name.empty() || !entity)
            continue;
        auto it = std::find_if(c.entities.begin(), c.entities.end(), [&](const Character::EntitySlot& s) { return s.name == name; });
        if (it == c.entities.end())
            it = c.entities.insert(c.entities.end(), Character::EntitySlot{name});
        it->bound = entity;
    }
}

bool AnimationSystem::setEntity(uint32_t record, std::string_view name, uint64_t entity)
{
    if (record >= characters_.size() || name.empty())
        return false;
    Character& c = leaderOf(record);
    auto it = std::find_if(c.entities.begin(), c.entities.end(), [&](const Character::EntitySlot& s) { return s.name == name; });
    if (it == c.entities.end()) {
        const std::vector<std::string>* names = c.graph ? &c.graph->definition().entityParameters : nullptr;
        if (!names || std::find(names->begin(), names->end(), name) == names->end())
            return false;
        it = c.entities.insert(c.entities.end(), Character::EntitySlot{std::string(name)});
    }
    it->scripted = entity;
    it->isScripted = true;
    return true;
}

bool AnimationSystem::resetEntity(uint32_t record, std::string_view name)
{
    if (record >= characters_.size())
        return false;
    Character& c = leaderOf(record);
    auto it = std::find_if(c.entities.begin(), c.entities.end(), [&](const Character::EntitySlot& s) { return s.name == name; });
    if (it == c.entities.end()) {
        const std::vector<std::string>* names = c.graph ? &c.graph->definition().entityParameters : nullptr;
        return names && std::find(names->begin(), names->end(), name) != names->end();
    }
    it->isScripted = false;
    it->scripted = 0;
    if (!it->bound)
        c.entities.erase(it);
    return true;
}

uint64_t AnimationSystem::entity(uint32_t record, std::string_view name) const
{
    if (record >= characters_.size())
        return 0;
    const Character::EntitySlot* s = slot(leaderOf(record), name);
    return s ? s->value() : 0;
}

void AnimationSystem::prepareIk(Character& c, float dt)
{
    followIk(c);
    Character::Ik& k = c.ik;
    k.feetNow = false;
    k.hasWorld = worldOf_ && worldOf_(c.record, k.world);
    if (!k.hasWorld)
        return;
    const float (&world)[3][4] = k.world;
    const anim::Pose& pose = c.pose;
    const float follow = dt > 0.0f && k.settled ? std::min(1.0f, 14.0f * dt) : 1.0f;  // smoothing (snap at the start)

    // Feet: each one's ground under it (its target smoothed), the hips down as far as the lower one
    // needs (applyIk places them).
    if (k.feetWeight > 0.0f && ground_ && findLegs(c)) {
        const float3 origin = {world[0][3], world[1][3], world[2][3]};  // the character's ground (its feet's plane)
        const float reach = std::max(k.feetReach, 0.01f);
        float target[2];
        float3 normal[2];
        float3* ankle = k.ankle;
        float* height = k.height;
        for (int side = 0; side < 2; ++side) {
            ankle[side] = toWorld(world, pose.model()[uint32_t(k.foot[side])].t);
            height[side] = ankle[side].y - origin.y;
            float3 point, n;
            const float3 from = {ankle[side].x, origin.y + reach, ankle[side].z};
            if (ground_(from, {0.0f, -2.0f * reach, 0.0f}, point, n) && n.y > 0.3f) {
                target[side] = std::clamp(point.y - origin.y, -reach, reach);
                normal[side] = normalize(n);
            } else {
                target[side] = 0.0f;
                normal[side] = {0.0f, 1.0f, 0.0f};
            }
            target[side] *= k.feetWeight;
            k.footOffset[side] += (target[side] - k.footOffset[side]) * follow;
            k.footNormal[side] = normalize(k.footNormal[side] + (normal[side] - k.footNormal[side]) * follow);
        }
        k.pelvisOffset += (std::min({target[0], target[1], 0.0f}) - k.pelvisOffset) * follow;
        k.settled = true;
        k.feetNow = true;
    }
}

void AnimationSystem::applyIk(Character& c, float)
{
    Character::Ik& k = c.ik;
    if (!k.hasWorld) {
        writeSkins(c);
        return;
    }
    const float (&world)[3][4] = k.world;
    float inverse[3][4];
    affineInverse(world, inverse);
    anim::Pose& pose = c.pose;
    const auto toModel = [&](float3 p) { return toWorld(inverse, p); };
    const float worldScale = length(float3{world[0][0], world[1][0], world[2][0]});

    if (k.feetNow) {
        const float3* ankle = k.ankle;
        const float* height = k.height;
        // The hips down (in the pelvis's parent's space).
        const uint32_t pelvis = uint32_t(k.pelvis);
        const int32_t up = c.skeleton->parent(pelvis);
        const anim::Xform parent = up >= 0 ? pose.model()[uint32_t(up)] : anim::Xform{};
        const float3 lowered = toModel(toWorld(world, pose.model()[pelvis].t) + float3{0.0f, k.pelvisOffset, 0.0f});
        pose.local(pelvis).t = anim::inverseRotate(parent.r, lowered - parent.t) * (1.0f / std::max(parent.s, 1e-6f));
        pose.calculateModelSpace();

        // Each foot onto its ground, tilted to it as it plants (the rest pose's ankle height: planted).
        const float3 upModel = directionToModel(inverse, {0.0f, 1.0f, 0.0f});
        for (int side = 0; side < 2; ++side) {
            const uint32_t foot = uint32_t(k.foot[side]);
            const float3 goal = toModel(ankle[side] + float3{0.0f, k.footOffset[side], 0.0f});
            const anim::Quat footRotation = pose.model()[foot].r;
            anim::solveTwoBone(pose, uint32_t(k.thigh[side]), uint32_t(k.calf[side]), foot, goal, 1.0f, false);
            const float rest = c.skeleton->referenceModel(foot).t.y * worldScale;
            const float planted = std::clamp(1.0f - (height[side] - rest) / 0.12f, 0.0f, 1.0f) * k.feetWeight;
            float3 slope = k.footNormal[side];
            const float tilt = dmath::acos(std::clamp(slope.y, -1.0f, 1.0f));
            constexpr float kMaxTilt = 0.6f;  // ~35 degrees
            if (tilt > kMaxTilt)
                slope = anim::rotate(anim::fromAxisAngle(cross(float3{0.0f, 1.0f, 0.0f}, slope), kMaxTilt), float3{0.0f, 1.0f, 0.0f});
            const anim::Quat align = anim::slerp(anim::Quat{}, anim::fromTo(upModel, directionToModel(inverse, slope)), planted);
            anim::setModelRotation(pose, foot, anim::mul(align, footRotation));
        }
    }

    // Aiming (the spine, until the bone's axis points at the target), then looking (the spine, neck
    // and head; while aiming, the spine's shares going to the aim), then the aim's hold (the weapon
    // hand to its place, the other hand keeping its grip), then the reaches (hands, after the aim
    // and the look turned the arms).
    const float aim = k.aimBone >= 0 && !k.aimChain.empty() ? std::min(k.aimWeight, 1.0f) : 0.0f;
    if (aim > 0.0f) {
        k.aimForward = length(k.aimAxis) > 1e-6f ? normalize(k.aimAxis)
                                                 : anim::inverseRotate(pose.model()[uint32_t(k.aimBone)].r, {0.0f, 0.0f, 1.0f});
        if (k.aimSpine > 0.0f)
            anim::solveAim(pose, k.aimChain, {}, uint32_t(k.aimBone), k.aimForward, toModel(k.aimTarget), 1.4f, aim * k.aimSpine, 4);
    }
    if (k.lookWeight > 0.0f && !k.lookChain.empty()) {
        std::vector<float>& shares = k.shares;
        shares = k.lookShares;
        if (aim > 0.0f)
            for (size_t i = 0; i < shares.size(); ++i)
                if (std::find(k.aimChain.begin(), k.aimChain.end(), k.lookChain[i]) != k.aimChain.end())
                    shares[i] *= 1.0f - aim;
        anim::solveLook(pose, k.lookChain, shares, k.lookForward, toModel(k.lookTarget), 1.3f, std::min(k.lookWeight, 1.0f));
    }
    if (aim > 0.0f && k.aimHeld)
        anim::solveHold(pose, uint32_t(k.aimBone), k.aimForward, toModel(k.aimHold), toModel(k.aimTarget), k.aimSupport, aim);
    for (const Character::Reach& r : k.reaches) {
        const int32_t mid = c.skeleton->parent(r.bone);
        const int32_t upper = mid >= 0 ? c.skeleton->parent(uint32_t(mid)) : -1;
        if (upper >= 0)
            anim::solveTwoBone(pose, uint32_t(upper), uint32_t(mid), r.bone, toModel(r.target), std::min(r.weight, 1.0f), true);
    }
    writeSkins(c);
}

void AnimationSystem::writeSkins(Character& c)
{
    writeSkin(c, c.record);
    const float* shared = skinning_->matrices(c.record);
    for (const uint32_t r : c.followers)
        writeSkin(c, r, shared);
}

AnimationSystem::Character& AnimationSystem::leaderOf(uint32_t record)
{
    Character& c = characters_[record];
    return c.leader >= 0 ? characters_[uint32_t(c.leader)] : c;
}

const AnimationSystem::Character& AnimationSystem::leaderOf(uint32_t record) const
{
    const Character& c = characters_[record];
    return c.leader >= 0 ? characters_[uint32_t(c.leader)] : c;
}

bool AnimationSystem::setFootIkWeight(uint32_t record, float weight)
{
    if (record >= characters_.size() || !findLegs(leaderOf(record)))
        return false;
    leaderOf(record).ik.feetWeight = std::clamp(weight, 0.0f, 1.0f);
    leaderOf(record).ik.feetScripted = true;
    return true;
}

float AnimationSystem::footIkWeight(uint32_t record) const
{
    return record < characters_.size() ? leaderOf(record).ik.feetWeight : 0.0f;
}

bool AnimationSystem::lookAt(uint32_t record, float3 target, float weight, std::string_view tip)
{
    if (record >= characters_.size())
        return false;
    Character& c = leaderOf(record);
    const int32_t t = c.skeleton->findBone(std::string(tip).c_str());
    if (t < 0)
        return false;
    Character::Ik& k = c.ik;
    k.lookScripted |= !followingTargets_;
    if (k.lookChain.empty() || k.lookChain.back() != uint32_t(t)) {
        k.lookChain = anim::lookChain(*c.skeleton, uint32_t(t));
        k.lookShares.clear();
        for (const uint32_t b : k.lookChain) {
            std::string n = c.skeleton->name(b);
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
            k.lookShares.push_back(n.find("head") != std::string::npos ? 2.0f : n.find("neck") != std::string::npos ? 1.5f : 1.0f);
        }
        // The tip's forward: the model's +Z (glTF's front) in the tip's space at rest.
        k.lookForward = anim::inverseRotate(c.skeleton->referenceModel(uint32_t(t)).r, {0.0f, 0.0f, 1.0f});
    }
    k.lookTarget = target;
    k.lookWeight = std::clamp(weight, 0.0f, 1.0f);
    return true;
}

bool AnimationSystem::reach(uint32_t record, std::string_view bone, float3 target, float weight)
{
    if (record >= characters_.size())
        return false;
    Character& c = leaderOf(record);
    const int32_t b = c.skeleton->findBone(std::string(bone).c_str());
    if (b < 0 || c.skeleton->parent(uint32_t(b)) < 0 || c.skeleton->parent(uint32_t(c.skeleton->parent(uint32_t(b)))) < 0)
        return false;
    if (!followingTargets_ && std::find(c.ik.scriptedReaches.begin(), c.ik.scriptedReaches.end(), uint32_t(b)) == c.ik.scriptedReaches.end())
        c.ik.scriptedReaches.push_back(uint32_t(b));
    std::vector<Character::Reach>& reaches = c.ik.reaches;
    auto it = std::find_if(reaches.begin(), reaches.end(), [&](const Character::Reach& r) { return r.bone == uint32_t(b); });
    if (weight <= 0.0f) {
        if (it != reaches.end())
            reaches.erase(it);
        return true;
    }
    if (it == reaches.end()) {
        if (reaches.size() >= 4)
            return false;
        it = reaches.insert(reaches.end(), Character::Reach{uint32_t(b)});
    }
    it->target = target;
    it->weight = std::min(weight, 1.0f);
    return true;
}

bool AnimationSystem::aimAt(uint32_t record, float3 target, float weight, std::string_view bone, float3 axis, const float3* hold,
                            std::string_view support)
{
    if (record >= characters_.size())
        return false;
    Character& c = leaderOf(record);
    const int32_t b = c.skeleton->findBone(std::string(bone).c_str());
    if (b < 0)
        return false;
    Character::Ik& k = c.ik;
    k.aimScripted |= !followingTargets_;
    if (k.aimBone != b) {
        std::vector<uint32_t> chain = anim::aimChain(*c.skeleton, uint32_t(b));
        if (chain.empty())
            return false;  // (the aim as it was)
        k.aimChain = std::move(chain);
        k.aimBone = b;
    }
    k.aimTarget = target;
    k.aimAxis = axis;
    k.aimHeld = hold != nullptr;
    k.aimSpine = 1.0f;  // (the graph's IK sets its own after the call)
    k.aimHold = hold ? *hold : float3{};
    k.aimSupport = hold && !support.empty() ? c.skeleton->findBone(std::string(support).c_str()) : -1;
    k.aimWeight = std::clamp(weight, 0.0f, 1.0f);
    return true;
}

bool AnimationSystem::aiming(uint32_t record, uint32_t& bone, float3& forward) const
{
    if (record >= characters_.size())
        return false;
    const Character::Ik& k = leaderOf(record).ik;
    if (k.aimWeight <= 0.0f || k.aimBone < 0 || k.aimChain.empty())
        return false;
    bone = uint32_t(k.aimBone);
    forward = k.aimForward;
    return true;
}

bool AnimationSystem::ikActive() const
{
    for (const Character& c : characters_)
        if (c.leader < 0 && c.ik.active())
            return true;
    return false;
}

void AnimationSystem::writeSkin(Character& c, uint32_t record, const float* shared)
{
    if (shared) {  // (the same pose and skeleton: the same matrices; its own mesh's bounds)
        std::memcpy(skinning_->matrices(record), shared, sizeof(float) * 12 * c.pose.boneCount());
        skinning_->setBounds(record, boundsOf(c.pose, *characters_[record].boneRadii));
    } else {
        skinning_->setBounds(record, writeMatrices(record, c.pose, *characters_[record].boneRadii));
    }
    skinning_->markPosed(record);
    // Hidden bones: where their vertices collapse to, for the eye (the hidden bone's joint).
    skinning_->setHidden(record, !c.collapse.empty());
    if (c.collapse.empty())
        return;
    float* out = skinning_->collapse(record);
    const std::vector<anim::Xform>& model = c.pose.model();
    for (uint32_t b = 0; b < c.collapse.size(); ++b) {
        const int32_t to = c.collapse[b];
        const float3 joint = to >= 0 ? model[uint32_t(to)].t : float3{0.0f, 0.0f, 0.0f};
        out[b * 4 + 0] = joint.x, out[b * 4 + 1] = joint.y, out[b * 4 + 2] = joint.z;
        out[b * 4 + 3] = to >= 0 ? 1.0f : 0.0f;
    }
}

float4 AnimationSystem::skinPose(uint32_t record, uint32_t mesh, const anim::Pose& pose)
{
    static const std::vector<float> none;
    const float4 b = writeMatrices(record, pose, mesh < meshBoneRadii_.size() ? meshBoneRadii_[mesh] : none);
    skinning_->setBounds(record, b);
    return b;
}

float4 AnimationSystem::writeMatrices(uint32_t record, const anim::Pose& pose, const std::vector<float>& radii)
{
    // Skinning matrices (model bone x inverse bind) and the animated bounds.
    const anim::Skeleton* skeleton = pose.skeleton();
    float* out = skinning_->matrices(record);
    const std::vector<anim::Xform>& model = pose.model();
    for (uint32_t b = 0; b < skeleton->boneCount(); ++b) {
        float m[12];
        anim::toMatrix3x4(model[b], m);
        mul3x4(m, skeleton->inverseBind(b), out + b * 12);
    }
    return boundsOf(pose, radii);
}

float4 AnimationSystem::boundsOf(const anim::Pose& pose, const std::vector<float>& radii)
{
    // The bones a mesh's vertices hang from, each with its vertices' reach.
    const anim::Skeleton* skeleton = pose.skeleton();
    const std::vector<anim::Xform>& model = pose.model();
    float3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
    const auto radius = [&](uint32_t b) { return b < radii.size() ? radii[b] : 0.0f; };
    for (uint32_t b = 0; b < skeleton->boneCount(); ++b) {
        const float r = radius(b);
        if (r > 0.0f) {
            const float3 p = model[b].t;
            lo = {std::min(lo.x, p.x - r), std::min(lo.y, p.y - r), std::min(lo.z, p.z - r)};
            hi = {std::max(hi.x, p.x + r), std::max(hi.y, p.y + r), std::max(hi.z, p.z + r)};
        }
    }
    const float3 center = (lo + hi) * 0.5f;
    float extent = 0.0f;
    for (uint32_t b = 0; b < skeleton->boneCount(); ++b)
        if (radius(b) > 0.0f)
            extent = std::max(extent, length(model[b].t - center) + radius(b));
    return {center.x, center.y, center.z, extent};
}

void AnimationSystem::setEditorPreview(uint32_t record, bool preview)
{
    if (record < characters_.size())
        leaderOf(record).preview = preview;
}

bool AnimationSystem::editorPreview(uint32_t record) const
{
    return record < characters_.size() && leaderOf(record).preview;
}

float AnimationSystem::playedTime(uint32_t record) const
{
    return record < characters_.size() ? leaderOf(record).played : 0.0f;
}

void AnimationSystem::update(float dt, bool editing)
{
    const auto stepOf = [&](const Character& c) { return editing && !c.preview ? 0.0f : dt; };
    FX_ZONE_CAT("AnimationSystem::update", Animation);
    if (characters_.empty())
        return;
    const uint64_t t0 = SDL_GetTicksNS();
    // Shown instances only (a pool's unused slots are hidden); a group is shown when any member is.
    const auto& rows = pak_->instances();
    for (Character& c : characters_) {
        const uint32_t slot = skinning_->instanceOfRecord(c.record);
        c.active = slot < rows.size() && !(rows[slot].flags & fpak::kInstanceHidden);
        skinning_->setActive(c.record, c.active);
    }
    std::vector<Character*> work;
    for (Character& c : characters_) {
        if (c.leader >= 0)
            continue;
        bool shown = c.active;
        for (const uint32_t r : c.followers)
            shown = shown || characters_[r].active;
        if (shown)
            work.push_back(&c);
    }
    if (work.size() < 4) {
        for (Character* c : work)
            evaluate(*c, stepOf(*c));
    } else {
        enki::TaskSet task(uint32_t(work.size()), [&](enki::TaskSetPartition range, uint32_t) {
            FX_ZONE_CAT("Evaluate characters", Animation);
            for (uint32_t i = range.start; i < range.end; ++i)
                evaluate(*work[i], stepOf(*work[i]));
        });
        jobs::scheduler().AddTaskSetToPipe(&task);
        jobs::scheduler().WaitforTask(&task);
    }
    // IK: what reads the world (targets, the feet's rays) here, on the main thread; the solves and
    // the skinning on the job threads.
    std::vector<Character*> ik;
    for (Character* c : work)
        if (c->ik.active()) {
            prepareIk(*c, stepOf(*c));
            ik.push_back(c);
        }
    if (ik.size() < 4) {
        for (Character* c : ik)
            applyIk(*c, stepOf(*c));
    } else {
        enki::TaskSet task(uint32_t(ik.size()), [&](enki::TaskSetPartition range, uint32_t) {
            FX_ZONE_CAT("Character IK", Animation);
            for (uint32_t i = range.start; i < range.end; ++i)
                applyIk(*ik[i], stepOf(*ik[i]));
        });
        jobs::scheduler().AddTaskSetToPipe(&task);
        jobs::scheduler().WaitforTask(&task);
    }
    updateMs_ = float(double(SDL_GetTicksNS() - t0) * 1e-6);
}

}  // namespace fx
