#include "editor/scene_edits.h"
#include "editor/material_library.h"
#include "editor/prefab.h"

#include "assets/scene_pak.h"
#include "assets/acoustic_material.h"
#include "assets/sound_asset.h"
#include "anim/anim_graph_compiler.h"
#include "anim/animation_system.h"
#include "anim/bone_entities.h"
#include "assets/surface_sound.h"
#include "core/json.h"
#include "core/log.h"
#include "level/level_mask.h"
#include "level/level_meshes.h"
#include "scene/scene.h"

#include <SDL3/SDL_iostream.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace fx {

void readLightJson(const Json& j, Light& l)
{
    const std::string type = j.getString("type");
    l.type = type == "directional" ? 2u : type == "spot" ? 1u : 0u;
    l.color[0] = l.color[1] = l.color[2] = 1.0f;
    if (const Json* c = j.get("color"); c && c->array.size() == 3)
        for (int k = 0; k < 3; ++k)
            l.color[k] = float(c->array[k].number);
    l.intensity = float(j.getNumber("intensity", 1.0));
    l.range = float(j.getNumber("range", 0.0));
    l.innerAngle = float(j.getNumber("inner", 20.0));
    l.outerAngle = float(j.getNumber("outer", 30.0));
    l.flags = j.getBool("shadows") ? 1u : 0u;
}

void writeLightJson(JsonWriter& w, const Light& l)
{
    w.beginObject("light").value("type", l.type == 2 ? "directional" : l.type == 1 ? "spot" : "point").beginArray("color");
    for (int k = 0; k < 3; ++k)
        w.value(nullptr, double(l.color[k]));
    w.endArray().value("intensity", double(l.intensity)).value("range", double(l.range));
    if (l.type == 1)
        w.value("inner", double(l.innerAngle)).value("outer", double(l.outerAngle));
    w.value("shadows", (l.flags & 1u) != 0).endObject();
}

const char* colliderShapeName(uint32_t shape) { return shape == 3 ? "capsule" : shape == 2 ? "mesh" : shape == 1 ? "sphere" : "box"; }
uint32_t colliderShapeOf(const std::string& name)
{
    return name == "capsule" ? 3u : name == "mesh" ? 2u : name == "sphere" ? 1u : 0u;
}

void readBodyJson(const Json& j, PhysicsBody& b)
{
    b = {};
    b.mass = std::max(float(j.getNumber("mass", 1.0)), 1e-4f);
    b.linearDamping = float(j.getNumber("linearDamping", 0.0));
    b.angularDamping = float(j.getNumber("angularDamping", 0.05));
    b.gravityScale = float(j.getNumber("gravityScale", 1.0));
    b.flags = j.getBool("kinematic") ? 1u : 0u;
}

void writeBodyJson(JsonWriter& w, const PhysicsBody& b)
{
    w.beginObject("body").value("mass", double(b.mass)).value("linearDamping", double(b.linearDamping))
        .value("angularDamping", double(b.angularDamping)).value("gravityScale", double(b.gravityScale))
        .value("kinematic", (b.flags & 1u) != 0).endObject();
}

void readColliderJson(const Json& j, Collider& c)
{
    c = {};
    c.shape = colliderShapeOf(j.getString("shape"));
    c.size[0] = c.size[1] = c.size[2] = 1.0f;
    for (const char* key : {"center", "size"})
        if (const Json* v = j.get(key); v && v->array.size() == 3)
            for (int k = 0; k < 3; ++k)
                (key[0] == 'c' ? c.center : c.size)[k] = float(v->array[size_t(k)].number);
    c.friction = float(j.getNumber("friction", 0.6));
    c.bounciness = float(j.getNumber("bounciness", 0.0));
    c.layer = uint32_t(std::clamp(j.getNumber("layer", 0.0), 0.0, 31.0));
    if (const Json* v = j.get("ignore"); v && v->type == Json::Type::Array)  // the layers it passes through
        for (const Json& l : v->array)
            if (l.number >= 0.0 && l.number < 32.0)
                c.ignore |= 1u << uint32_t(l.number);
    c.flags = j.getBool("trigger") ? 1u : 0u;
}

void writeColliderJson(JsonWriter& w, const Collider& c)
{
    w.beginObject("collider").value("shape", colliderShapeName(c.shape));
    for (const char* key : {"center", "size"}) {
        w.beginArray(key);
        for (int k = 0; k < 3; ++k)
            w.value(nullptr, double((key[0] == 'c' ? c.center : c.size)[k]));
        w.endArray();
    }
    w.value("friction", double(c.friction)).value("bounciness", double(c.bounciness));
    if (c.layer)
        w.value("layer", double(c.layer));
    if (c.ignore) {
        w.beginArray("ignore");
        for (uint32_t l = 0; l < 32; ++l)
            if (c.ignore & (1u << l))
                w.value(nullptr, double(l));
        w.endArray();
    }
    if (c.flags & 1u)
        w.value("trigger", true);
    w.endObject();
}

AudioSource defaultAudioSource()
{
    AudioSource a = {};
    a.flags = 1u | 4u | 16u | 64u | 128u | 256u;  // play on start, 3D, air, occlusion, transmission, reflections
    a.volume = a.pitch = 1.0f;
    a.minDistance = 1.0f;
    a.maxDistance = 50.0f;
    return a;
}

void readAudioSourceJson(const Json& j, AudioSource& a)
{
    a = defaultAudioSource();
    a.sound = soundId(j.getString("sound"));
    const auto bit = [&](const char* key, uint32_t b) {
        if (const Json* v = j.get(key); v && v->type == Json::Type::Bool)
            a.flags = v->b ? (a.flags | b) : (a.flags & ~b);
    };
    bit("playOnStart", 1u);
    bit("loop", 2u);
    bit("spatial", 4u);
    bit("ownSettings", 8u);
    bit("airAbsorption", 16u);
    bit("occlusion", 64u);
    bit("transmission", 128u);
    bit("reflections", 256u);
    if (const Json* v = j.get("vary"); v && v->type == Json::Type::Bool)
        a.flags = v->b ? (a.flags & ~512u) : (a.flags | 512u);
    if (j.getString("rolloff") == "linear")
        a.flags |= 32u;
    a.volume = std::max(float(j.getNumber("volume", 1.0)), 0.0f);
    a.pitch = std::clamp(float(j.getNumber("pitch", 1.0)), 0.01f, 8.0f);
    a.minDistance = std::max(float(j.getNumber("minDistance", 1.0)), 0.01f);
    a.maxDistance = std::max(float(j.getNumber("maxDistance", 50.0)), a.minDistance);
    const std::string bus = j.getString("bus", "sfx");
    for (uint32_t b = 0; b < 5; ++b)
        if (bus == kSoundBuses[b])
            a.bus = b;
}

void writeAudioSourceJson(JsonWriter& w, const AudioSource& a)
{
    w.beginObject("audioSource").value("sound", soundPath(a.sound)).value("playOnStart", (a.flags & 1u) != 0)
        .value("loop", (a.flags & 2u) != 0).value("spatial", (a.flags & 4u) != 0).value("ownSettings", (a.flags & 8u) != 0)
        .value("airAbsorption", (a.flags & 16u) != 0).value("rolloff", (a.flags & 32u) ? "linear" : "inverse")
        .value("occlusion", (a.flags & 64u) != 0).value("transmission", (a.flags & 128u) != 0).value("reflections", (a.flags & 256u) != 0)
        .value("vary", (a.flags & 512u) == 0).value("volume", double(a.volume)).value("pitch", double(a.pitch)).value("minDistance", double(a.minDistance))
        .value("maxDistance", double(a.maxDistance)).value("bus", kSoundBuses[std::min(a.bus, 4u)]).endObject();
}

bool migrateAnimatorIk(const Json& j, AnimatorEdit& a)
{
    const bool footIk = j.getBool("footIk");
    const Json* t = j.get("targets");
    const bool targets = t && t->isObject();
    if (!footIk && !targets)
        return false;
    const auto key = [&](const char* name) { return targets ? t->getString(name) : std::string(); };
    const std::string look = key("look"), aim = key("aim"), hold = key("hold"), left = key("leftHand"), right = key("rightHand");
    const auto bind = [&](const char* name, const std::string& entity) {
        if (!entity.empty() && std::none_of(a.entities.begin(), a.entities.end(), [&](const auto& e) { return e.first == name; }))
            a.entities.push_back({name, entity});
    };
    JsonWriter w;
    w.beginObject();
    if (footIk)
        w.beginObject("feet").value("reach", double(std::clamp(float(j.getNumber("footReach", 0.45)), 0.01f, 2.0f))).endObject();
    if (!look.empty()) {
        w.beginObject("look").value("target", "$LookTarget").endObject();
        bind("LookTarget", look);
    }
    if (!aim.empty() || !hold.empty()) {
        w.beginObject("aim");
        if (!aim.empty())
            w.value("target", "$AimTarget");
        if (!hold.empty())
            w.value("hold", "$AimHold");
        w.value("bone", t->getString("aimBone").empty() ? std::string("hand_r") : t->getString("aimBone"));
        if (const Json* axis = t->get("aimAxis"); axis && axis->type == Json::Type::Array && axis->array.size() == 3) {
            w.beginArray("axis");
            for (const Json& v : axis->array)
                w.value(nullptr, v.number);
            w.endArray();
        }
        w.endObject();
        bind("AimTarget", aim);
        bind("AimHold", hold);
    }
    if (!left.empty() || !right.empty()) {  // (the hands found by name, as these were)
        w.beginArray("reach");
        if (!left.empty())
            w.beginObject().value("bone", "left hand").value("target", "$LeftHandTarget").endObject();
        if (!right.empty())
            w.beginObject().value("bone", "right hand").value("target", "$RightHandTarget").endObject();
        w.endArray();
        bind("LeftHandTarget", left);
        bind("RightHandTarget", right);
    }
    w.endObject();
    a.ik = w.str() == "{}" ? std::string() : w.str();
    return true;
}

std::shared_ptr<const anim::IkSettings> ownIkOf(const AnimatorEdit& a)
{
    if (a.ik.empty())
        return nullptr;
    std::vector<std::string> names;
    for (const auto& [name, key] : a.entities)
        names.push_back(name);
    // (its targets may name bindings since cleared: still parsed, they follow nothing)
    for (const char* name : {"LookTarget", "AimTarget", "AimHold", "LeftHandTarget", "RightHandTarget"})
        if (std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(name);
    auto ik = std::make_shared<anim::IkSettings>();
    std::vector<anim::CompileMessage> errors;
    try {
        if (anim::parseIk(Json::parse(a.ik), names, nullptr, *ik, errors))
            return ik;
    } catch (const std::exception& e) {
        errors.push_back({"ik", e.what()});
    }
    FX_WARN("animator: its own IK: %s", errors.empty() ? "?" : errors[0].text.c_str());
    return nullptr;
}

void readAnimatorJson(const Json& j, AnimatorEdit& a)
{
    a.graph = j.getString("graph");
    a.editorPreview = j.getBool("editorPreview");
    a.entities.clear();
    if (const Json* e = j.get("entities"); e && e->isObject())
        for (const auto& [name, v] : e->object)
            if (v.type == Json::Type::String && !v.str.empty())
                a.entities.push_back({name, v.str});
    a.ik.clear();
    if (const Json* ik = j.get("ik"); ik && ik->isObject() && !ik->object.empty()) {
        a.ik = ik->dump(0);
    }
    if (a.ik.empty())
        migrateAnimatorIk(j, a);
    a.params.clear();
    if (const Json* p = j.get("params"); p && p->isObject())
        for (const auto& [name, v] : p->object)
            if (v.type == Json::Type::Number || v.type == Json::Type::Bool)
                a.params.push_back({name, v.type == Json::Type::Bool ? (v.b ? 1.0f : 0.0f) : float(v.number)});
}

void writeAnimatorJson(JsonWriter& w, const AnimatorEdit& a)
{
    w.beginObject("animator").value("graph", a.graph);
    if (a.editorPreview)
        w.value("editorPreview", true);
    if (!a.entities.empty()) {
        w.beginObject("entities");
        for (const auto& [name, key] : a.entities)
            w.value(name.c_str(), key);
        w.endObject();
    }
    if (!a.ik.empty())
        w.raw("ik", a.ik);
    w.beginObject("params");
    for (const auto& [name, v] : a.params)
        w.value(name.c_str(), double(v));
    w.endObject().endObject();
}

void readAudioListenerJson(const Json& j, AudioListener& l)
{
    l = {};
    l.gain = std::max(float(j.getNumber("gain", 1.0)), 0.0f);
}

void writeAudioListenerJson(JsonWriter& w, const AudioListener& l)
{
    w.beginObject("audioListener").value("gain", double(l.gain)).endObject();
}

void readAcousticSurfaceJson(const Json& j, AcousticSurface& a)
{
    a = {};
    a.flags = (j.getBool("excluded") ? 1u : 0u) | (j.getBool("own") ? 2u : 0u) | (j.getBool("moving") ? 4u : 0u);
    std::string preset = "generic";
    AcousticParams p;
    readAcousticJson(j, preset, p);
    a.preset = acousticPresetIndex(preset);
    std::memcpy(a.absorption, p.absorption, sizeof(a.absorption));
    a.scattering = p.scattering;
    std::memcpy(a.transmission, p.transmission, sizeof(a.transmission));
    a.surface = surfacePresetIndex(j.getString("surface"));
    if (a.surface)
        a.flags |= 8u;
    a.impactGain = std::clamp(float(j.getNumber("impactGain", 1.0)), 0.0f, 4.0f);
    a.impactPitch = std::clamp(float(j.getNumber("impactPitch", 1.0)), 0.25f, 4.0f);
}

void writeAcousticSurfaceJson(JsonWriter& w, const AcousticSurface& a)
{
    AcousticParams p;
    std::memcpy(p.absorption, a.absorption, sizeof(p.absorption));
    p.scattering = a.scattering;
    std::memcpy(p.transmission, a.transmission, sizeof(p.transmission));
    w.beginObject("acoustics").value("excluded", (a.flags & 1u) != 0).value("own", (a.flags & 2u) != 0).value("moving", (a.flags & 4u) != 0);
    writeAcousticFields(w, acousticPresetName(a.preset), p);
    if (a.flags & 8u)
        w.value("surface", surfacePresetName(a.surface)).value("impactGain", double(a.impactGain)).value("impactPitch", double(a.impactPitch));
    w.endObject();
}

void readOverrides(const Json& j, std::vector<PrefabOverride>& out)
{
    for (const auto& [id, o] : j.object) {
        PrefabOverride ov;
        ov.id = id;
        if (const Json* l = o.get("local"); l && l->array.size() == 12) {
            ov.hasLocal = true;
            for (int k = 0; k < 12; ++k)
                ov.local[k / 4][k % 4] = float(l->array[size_t(k)].number);
        }
        if (const Json* h = o.get("hidden"))
            ov.hidden = h->b;
        if (const Json* m = o.get("material"))
            ov.material = m->str;
        if (const Json* n = o.get("name"))
            ov.name = n->str;
        if (const Json* l = o.get("light"); l && l->isObject()) {
            ov.light = Light{};
            readLightJson(*l, *ov.light);
        }
        if (const Json* c = o.get("camera"); c && c->isObject())
            ov.camera = CameraComponent{float(c->getNumber("fov", 60.0)), float(c->getNumber("near", 0.05)), c->getBool("main") ? 1u : 0u};
        if (const Json* b = o.get("body"); b && b->isObject()) {
            ov.body = PhysicsBody{};
            readBodyJson(*b, *ov.body);
        }
        if (const Json* c = o.get("collider"); c && c->isObject()) {
            ov.collider = Collider{};
            readColliderJson(*c, *ov.collider);
        }
        if (const Json* a = o.get("audioSource"); a && a->isObject()) {
            ov.audioSource = AudioSource{};
            readAudioSourceJson(*a, *ov.audioSource);
        }
        if (const Json* l = o.get("audioListener"); l && l->isObject()) {
            ov.audioListener = AudioListener{};
            readAudioListenerJson(*l, *ov.audioListener);
        }
        if (const Json* a = o.get("acoustics"); a && a->isObject()) {
            ov.acoustics = AcousticSurface{};
            readAcousticSurfaceJson(*a, *ov.acoustics);
        }
        if (o.getBool("vfx")) {  // the entity's particle and decal components replace the prefab's (none: "vfx": true alone)
            ov.vfx = VfxSet{};
            readVfxJson(o, *ov.vfx);
        }
        if (o.get("scripts")) {  // the entity's script components replace the prefab's ("scripts": [] for none)
            ov.scripts = std::vector<ScriptEntry>{};
            readScriptsJson(o, *ov.scripts);
        }
        ov.removed = o.getBool("removed");
        out.push_back(std::move(ov));
    }
}

void writeOverrides(JsonWriter& w, const std::vector<PrefabOverride>& list)
{
    w.beginObject("overrides");
    for (const PrefabOverride& o : list) {
        w.beginObject(o.id.c_str());
        if (o.removed)
            w.value("removed", true);
        if (o.hasLocal) {
            w.beginArray("local");
            for (int k = 0; k < 12; ++k)
                w.value(nullptr, double(o.local[k / 4][k % 4]));
            w.endArray();
        }
        if (o.hidden)
            w.value("hidden", *o.hidden);
        if (o.material)
            w.value("material", *o.material);
        if (o.name)
            w.value("name", *o.name);
        if (o.light)
            writeLightJson(w, *o.light);
        if (o.camera)
            w.beginObject("camera").value("fov", double(o.camera->fov)).value("near", double(o.camera->nearClip))
                .value("main", (o.camera->flags & 1u) != 0).endObject();
        if (o.body)
            writeBodyJson(w, *o.body);
        if (o.collider)
            writeColliderJson(w, *o.collider);
        if (o.audioSource)
            writeAudioSourceJson(w, *o.audioSource);
        if (o.audioListener)
            writeAudioListenerJson(w, *o.audioListener);
        if (o.acoustics)
            writeAcousticSurfaceJson(w, *o.acoustics);
        if (o.vfx) {
            w.value("vfx", true);
            writeVfxJson(w, *o.vfx);
        }
        if (o.scripts) {
            if (o.scripts->empty())
                w.beginArray("scripts").endArray();
            else
                writeScriptsJson(w, *o.scripts);
        }
        w.endObject();
    }
    w.endObject();
}

// A wall's parameters as JSON (the edits, the bridge's messages, the inspector) and back.
void writeWall(JsonWriter& w, const char* key, const level::Wall& wall)
{
    w.beginObject(key).beginArray("points");
    for (const float2 p : wall.points)
        w.beginArray().value(nullptr, double(p.x)).value(nullptr, double(p.y)).endArray();
    w.endArray().value("closed", wall.closed).value("height", double(wall.height)).value("thickness", double(wall.thickness))
        .value("align", uint32_t(wall.align)).value("floor", wall.floor).value("ceiling", wall.ceiling).value("slab", double(wall.slab))
        .value("sideA", wall.sideA).value("sideB", wall.sideB).value("top", wall.top).value("floorMaterial", wall.floorMaterial)
        .value("ceilingMaterial", wall.ceilingMaterial).endObject();
}

// (exported: the bridge's messages read walls the same way)
void readWallFields(const Json& j, level::Wall& wall)
{
    if (const Json* pts = j.get("points")) {
        wall.points.clear();
        for (const Json& p : pts->array)
            if (p.array.size() >= 2)
                wall.points.push_back({float(p.array[0].number), float(p.array[1].number)});
    }
    wall.closed = j.getBool("closed", wall.closed);
    wall.height = float(j.getNumber("height", wall.height));
    wall.thickness = float(j.getNumber("thickness", wall.thickness));
    wall.align = uint8_t(j.getNumber("align", wall.align));
    wall.floor = j.getBool("floor", wall.floor);
    wall.ceiling = j.getBool("ceiling", wall.ceiling);
    wall.slab = float(j.getNumber("slab", wall.slab));
    wall.sideA = j.getString("sideA", wall.sideA.c_str());
    wall.sideB = j.getString("sideB", wall.sideB.c_str());
    wall.top = j.getString("top", wall.top.c_str());
    wall.floorMaterial = j.getString("floorMaterial", wall.floorMaterial.c_str());
    wall.ceilingMaterial = j.getString("ceilingMaterial", wall.ceilingMaterial.c_str());
}

void readLevelParts(const Json& j, LevelParts& out)
{
    out = {};
    if (const Json* wj = j.get("wall"); wj && wj->isObject()) {
        level::Wall wall;
        readWallFields(*wj, wall);
        out.wall = wall;
    }
    if (const Json* o = j.get("opening"); o && o->isObject())
        out.opening = WallOpening{float(o->getNumber("distance", 1)), float(o->getNumber("sill", 0)), float(o->getNumber("width", 1)),
                                  float(o->getNumber("height", 2.1))};
    if (const Json* r = j.get("roof"); r && r->isObject()) {
        level::Roof roof;
        level::readRoof(*r, roof);
        out.roof = roof;
    }
    if (const Json* c = j.get("cut"); c && c->isObject()) {
        CutVolume v;
        const std::string shape = c->getString("shape", "box");
        for (uint32_t k = 0; k < level::kCutShapes; ++k)
            if (shape == level::kCutShapeNames[k])
                v.shape = level::CutShape(k);
        v.intersect = c->getBool("intersect");
        v.capMaterial = c->getString("cap");
        v.seed = uint32_t(c->getNumber("seed", 0));
        v.roughness = float(c->getNumber("roughness", 0.35));
        out.cut = v;
    }
    if (const Json* s = j.get("scatterSet"); s && s->isObject())
        out.scatterSet = ScatterSet{uint32_t(s->getNumber("seed", 1))};
    if (const Json* s = j.get("snapPoint"); s && s->isObject())
        out.snapPoint = SnapPoint{s->getString("tag")};
    if (const Json* v = j.get("volume"); v && v->isObject()) {
        LevelVolume vol;
        const std::string kind = v->getString("kind", "trigger");
        for (uint32_t k = 0; k < kVolumeKinds; ++k)
            if (kind == kVolumeKindNames[k])
                vol.kind = VolumeKind(k);
        vol.tag = v->getString("tag");
        vol.cost = float(v->getNumber("cost", vol.cost));
        out.volume = vol;
    }
    if (const Json* s = j.get("spline"); s && s->isObject()) {
        LevelSpline spline;
        level::readSpline(*s, spline.spline, spline.loft);
        out.spline = std::move(spline);
    }
    if (const Json* r = j.get("modifiers"); r && r->type == Json::Type::Array)
        for (const Json& mod : r->array)
            out.modifiers.push_back(level::modifierFromJson(mod));
    if (const Json* r = j.get("levelWear"); r && r->type == Json::Type::Array)
        for (const Json& rule : r->array)
            out.levelWear.push_back(level::wearFromJson(rule, level::WearKind::Edges));
}

void writeLevelParts(JsonWriter& w, const LevelParts& p)
{
    if (p.spline)
        level::writeSpline(w, "spline", p.spline->spline, p.spline->loft);
    if (p.scatterSet)
        w.beginObject("scatterSet").value("seed", p.scatterSet->seed).endObject();
    if (p.snapPoint)
        w.beginObject("snapPoint").value("tag", p.snapPoint->tag).endObject();
    if (p.volume)
        w.beginObject("volume").value("kind", kVolumeKindNames[uint32_t(p.volume->kind) % kVolumeKinds]).value("tag", p.volume->tag)
            .value("cost", double(p.volume->cost)).endObject();
    if (!p.modifiers.empty()) {
        w.beginArray("modifiers");
        for (const level::Modifier& mod : p.modifiers)
            level::writeModifierJson(w, nullptr, mod);
        w.endArray();
    }
    if (!p.levelWear.empty()) {
        w.beginArray("levelWear");
        for (const level::WearSettings& r : p.levelWear)
            level::writeWearJson(w, nullptr, r);
        w.endArray();
    }
    if (p.wall)
        writeWall(w, "wall", *p.wall);
    if (p.roof)
        level::writeRoof(w, "roof", *p.roof);
    if (p.opening)
        w.beginObject("opening").value("distance", double(p.opening->distance)).value("sill", double(p.opening->sill))
            .value("width", double(p.opening->width)).value("height", double(p.opening->height)).endObject();
    if (p.cut) {
        w.beginObject("cut").value("shape", level::kCutShapeNames[uint32_t(p.cut->shape) % level::kCutShapes])
            .value("intersect", p.cut->intersect).value("cap", p.cut->capMaterial);
        if (p.cut->shape == level::CutShape::Damage)
            w.value("seed", p.cut->seed).value("roughness", double(p.cut->roughness));
        w.endObject();
    }
}

LevelParts levelPartsOf(const EntityEdit& e)
{
    return {e.cut, e.wall, e.opening, e.modifiers, e.spline, e.levelWear, e.scatterSet, e.snapPoint, e.volume, e.roof};
}

void setLevelParts(EntityEdit& e, const LevelParts& p)
{
    e.cut = p.cut;
    e.wall = p.wall;
    e.opening = p.opening;
    e.modifiers = p.modifiers;
    e.spline = p.spline;
    e.levelWear = p.levelWear;
    e.scatterSet = p.scatterSet;
    e.snapPoint = p.snapPoint;
    e.volume = p.volume;
    e.roof = p.roof;
}

namespace {

bool readEdit(const Json& j, EntityEdit& e)
{
    if (!j.isObject())
        return false;
    e.slot = uint32_t(j.getNumber(j.get("index") ? "index" : "slot", ~0u));
    if (const Json* l = j.get("light"); l && l->isObject()) {
        e.hasLight = true;
        readLightJson(*l, e.light);
    }
    if (const Json* c = j.get("camera"); c && c->isObject()) {
        e.hasCamera = true;
        e.camera.fov = float(c->getNumber("fov", 60.0));
        e.camera.nearClip = float(c->getNumber("near", 0.05));
        e.camera.flags = c->getBool("main") ? 1u : 0u;
    }
    if (const Json* wb = j.get("water"); wb && wb->isObject()) {
        e.hasWater = true;
        readWaterBodyJson(*wb, e.water);
    }
    if (const Json* b = j.get("body"); b && b->isObject()) {
        e.hasBody = true;
        readBodyJson(*b, e.body);
    }
    if (const Json* c = j.get("collider"); c && c->isObject()) {
        e.hasCollider = true;
        readColliderJson(*c, e.collider);
    }
    if (const Json* a = j.get("audioSource"); a && a->isObject()) {
        e.hasAudioSource = true;
        readAudioSourceJson(*a, e.audioSource);
    }
    if (const Json* l = j.get("audioListener"); l && l->isObject()) {
        e.hasAudioListener = true;
        readAudioListenerJson(*l, e.audioListener);
    }
    if (const Json* a = j.get("acoustics"); a && a->isObject()) {
        e.hasAcoustics = true;
        readAcousticSurfaceJson(*a, e.acoustics);
    }
    if (const Json* a = j.get("animator"); a && a->isObject()) {
        e.hasAnimator = true;
        readAnimatorJson(*a, e.animator);
    }
    readVfxJson(j, e.vfx);
    readScriptsJson(j, e.scripts);
    if (const Json* data = j.get("data"))
        for (const Json& d : data->array) {
            DataComponent c;
            c.name = d.getString("name");
            c.size = uint32_t(d.getNumber("size"));
            c.align = uint32_t(d.getNumber("align", 4));
            const std::string hex = d.getString("bytes");
            for (size_t i = 0; i + 1 < hex.size() && c.bytes.size() < c.size; i += 2)
                c.bytes.push_back(uint8_t(std::stoul(hex.substr(i, 2), nullptr, 16)));
            c.bytes.resize(c.size);
            if (!c.name.empty() && c.size)
                e.data.push_back(std::move(c));
        }
    e.uid = uint32_t(j.getNumber("uid"));
    e.kind = j.getString("kind", "primitive");
    // A created mesh: any mesh of the scene by name ("primitive": built-ins, as older files wrote it).
    e.primitive = j.getString("mesh", j.getString("primitive", e.kind == "primitive" ? "cube" : "").c_str());
    e.name = j.getString("name");
    e.parent = j.getString("parent");
    e.material = j.getString("material");
    e.model = j.getString("model");
    e.modelNode = int32_t(j.getNumber("modelNode", -1));
    e.modelCamera = int32_t(j.getNumber("modelCamera", -1));
    e.prefab = j.getString("prefab");
    if (const Json* o = j.get("overrides"); o && o->isObject())
        readOverrides(*o, e.overrides);
    LevelParts parts;
    readLevelParts(j, parts);
    setLevelParts(e, parts);
    if (const Json* m = j.get("levelMask"); m && !m->str.empty()) {
        auto mask = std::make_shared<std::vector<uint8_t>>();
        if (level::maskFromText(m->str, *mask))
            e.levelMask = std::move(mask);
    }
    if (const Json* m = j.get("levelMesh"); m && !m->str.empty()) {
        auto mesh = std::make_shared<level::PolyMesh>();
        if (mesh->loadText(m->str))
            e.levelMesh = std::move(mesh);
        else
            FX_WARN("edits: a level mesh that can't be read (uid %u; skipped)", e.uid);
    }
    e.hidden = j.getBool("hidden");
    e.deleted = j.getBool("deleted");
    e.bones = j.getBool("bones");
    const Json* w = j.get(e.parent.empty() ? "world" : "local");
    if (!w || w->array.size() != 12)
        return false;
    for (int i = 0; i < 12; ++i)
        e.world[i / 4][i % 4] = float(w->array[i].number);
    return true;
}

void writeEdit(JsonWriter& w, const EntityEdit& e, bool cooked, const char* slotKey = "slot")
{
    w.beginObject();
    if (cooked) {
        w.value(slotKey, e.slot);
    } else {
        w.value("uid", e.uid).value("kind", e.kind);
        if (e.kind == "primitive")
            w.value("mesh", e.primitive);
        if (e.kind == "prefab")
            w.value("prefab", e.prefab);
    }
    if (!e.name.empty())
        w.value("name", e.name);
    if (!e.parent.empty())
        w.value("parent", e.parent);
    if (!e.material.empty())
        w.value("material", e.material);
    if (!e.model.empty()) {
        w.value("model", e.model);
        if (e.modelNode >= 0)
            w.value("modelNode", e.modelNode);
        if (e.modelCamera >= 0)
            w.value("modelCamera", e.modelCamera);
    }
    w.beginArray(e.parent.empty() ? "world" : "local");
    for (int i = 0; i < 12; ++i)
        w.value(nullptr, double(e.world[i / 4][i % 4]));
    w.endArray();
    w.value("hidden", e.hidden);
    if (e.deleted)
        w.value("deleted", true);
    if (e.bones)
        w.value("bones", true);
    if (e.hasLight)
        writeLightJson(w, e.light);
    if (e.kind == "prefab" && !e.overrides.empty())
        writeOverrides(w, e.overrides);
    if (e.hasCamera)
        w.beginObject("camera").value("fov", double(e.camera.fov)).value("near", double(e.camera.nearClip))
            .value("main", (e.camera.flags & 1u) != 0).endObject();
    if (e.hasWater)
        writeWaterBodyJson(w, "water", e.water);
    if (e.hasBody)
        writeBodyJson(w, e.body);
    if (e.hasCollider)
        writeColliderJson(w, e.collider);
    if (e.hasAudioSource)
        writeAudioSourceJson(w, e.audioSource);
    if (e.hasAudioListener)
        writeAudioListenerJson(w, e.audioListener);
    if (e.hasAcoustics)
        writeAcousticSurfaceJson(w, e.acoustics);
    if (e.hasAnimator)
        writeAnimatorJson(w, e.animator);
    if (e.levelMesh)
        w.value("levelMesh", e.levelMesh->saveText());
    if (e.levelMask)
        w.value("levelMask", level::maskToText(*e.levelMask));
    writeLevelParts(w, levelPartsOf(e));
    writeVfxJson(w, e.vfx);
    writeScriptsJson(w, e.scripts);
    if (!e.data.empty()) {
        w.beginArray("data");
        for (const DataComponent& d : e.data) {
            static const char* const kHex = "0123456789abcdef";
            std::string hex;
            for (const uint8_t b : d.bytes)
                hex += std::string{kHex[b >> 4], kHex[b & 15]};
            w.beginObject().value("name", d.name).value("size", d.size).value("align", d.align).value("bytes", hex).endObject();
        }
        w.endArray();
    }
    w.endObject();
}

}  // namespace

uint64_t dataComponentId(flecs::world& world, const DataComponent& d, bool create)
{
    ecs_world_t* w = world.c_ptr();
    if (ecs_entity_t e = ecs_lookup_path_w_sep(w, 0, d.name.c_str(), "::", "::", false)) {
        const ecs_type_info_t* ti = ecs_get_type_info(w, e);
        return ti && uint32_t(ti->size) == d.size && uint32_t(ti->alignment) == d.align ? e : 0;
    }
    if (!create || !d.size || !d.align)
        return 0;
    ecs_entity_desc_t ed = {};
    ed.name = d.name.c_str();
    ed.sep = "::";  // (C# full names use '.': one path element, as the scripts register them)
    ecs_component_desc_t cd = {};
    cd.entity = ecs_entity_init(w, &ed);
    cd.type.size = int32_t(d.size);
    cd.type.alignment = int32_t(d.align);
    return ecs_component_init(w, &cd);
}

void setDataComponents(flecs::entity e, const std::vector<DataComponent>& data)
{
    flecs::world world = e.world();
    for (const DataComponent& d : data)
        if (const uint64_t id = dataComponentId(world, d, true); id && d.bytes.size() == d.size)
            ecs_set_id(world.c_ptr(), e.id(), id, d.size, d.bytes.data());
}

std::string SceneEdits::pathFor(const char* scenePath)
{
    std::string p = scenePath;
    const size_t dot = p.find_last_of('.');
    const size_t slash = p.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        p.resize(dot);
    return p + ".edits.json";
}

bool SceneEdits::load(const std::string& path)
{
    instances.clear();
    created.clear();
    size_t size = 0;
    void* text = SDL_LoadFile(path.c_str(), &size);
    if (!text)
        return false;
    const std::string copy(static_cast<const char*>(text), size);
    SDL_free(text);
    return loadText(copy, path);
}

bool SceneEdits::loadText(std::string_view text, const std::string& path)
{
    instances.clear();
    created.clear();
    try {
        const Json root = Json::parse(text);
        if (const Json* list = root.get("instances"))
            for (const Json& j : list->array)
                if (EntityEdit e; readEdit(j, e))
                    instances.push_back(std::move(e));
        if (const Json* list = root.get("created"))
            for (const Json& j : list->array)
                if (EntityEdit e; readEdit(j, e))
                    created.push_back(std::move(e));
        if (const Json* list = root.get("lights"))
            for (const Json& j : list->array)
                if (EntityEdit e; readEdit(j, e))
                    lights.push_back(std::move(e));
        // Version 1: created entities had no uids (all primitives, no parents).
        nextUid = std::max(uint32_t(root.getNumber("nextUid", 1)), 1u);
        for (EntityEdit& e : created)
            if (e.uid == 0)
                e.uid = nextUid++;
            else
                nextUid = std::max(nextUid, e.uid + 1);
    } catch (const std::exception& e) {
        FX_ERROR("edits: %s: %s (ignored)", path.c_str(), e.what());
        instances.clear();
        created.clear();
        return false;
    }
    FX_INFO("edits: %s (%zu instances edited, %zu created)", path.c_str(), instances.size(), created.size());
    return true;
}

std::string SceneEdits::text() const
{
    JsonWriter w;
    w.beginObject().value("version", 2u).value("nextUid", nextUid).beginArray("instances");
    for (const EntityEdit& e : instances)
        writeEdit(w, e, true);
    w.endArray().beginArray("created");
    for (const EntityEdit& e : created)
        writeEdit(w, e, false);
    w.endArray().beginArray("lights");
    for (const EntityEdit& e : lights)
        writeEdit(w, e, true, "index");
    w.endArray().endObject();
    return w.str();
}

bool SceneEdits::save(const std::string& path) const
{
    // Write-then-rename so a crash never leaves a truncated file.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text() << '\n';
        if (!out)
            return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

void SceneEdits::expandPrefabs(const std::string& projectDir)
{
    uint32_t nextExpanded = kExpandedUids;
    std::vector<EntityEdit> out;
    for (EntityEdit& e : created) {
        if (e.kind != "prefab") {
            out.push_back(std::move(e));
            continue;
        }
        Prefab prefab;
        std::string error;
        if (projectDir.empty() || !readPrefab(projectDir + "/" + e.prefab, prefab, error, projectDir)) {
            FX_WARN("edits: prefab %s: %s (an empty stands in)", e.prefab.c_str(), error.c_str());
            e.kind = "empty";
            e.prefab.clear();
            out.push_back(std::move(e));
            continue;
        }
        const uint32_t rootUid = e.uid;
        const auto overrideOf = [&](const std::string& id) -> const PrefabOverride* {
            for (const PrefabOverride& o : e.overrides)
                if (o.id == id)
                    return &o;
            return nullptr;
        };
        // One edit per prefab entity: the root is this edit (its transform the instance's).
        const auto fillFrom = [&](const PrefabEntity& def, EntityEdit& x) {
            const PrefabOverride* o = overrideOf(def.id);
            x.kind = def.kind == "mesh" ? "primitive" : def.kind;
            x.primitive = def.kind == "mesh" ? def.mesh : "";
            x.material = o && o->material ? *o->material : def.material;
            x.hasLight = def.light.has_value() || (o && o->light);
            if (x.hasLight)
                x.light = o && o->light ? *o->light : *def.light;
            x.hasCamera = def.camera.has_value() || (o && o->camera);
            if (x.hasCamera)
                x.camera = o && o->camera ? *o->camera : *def.camera;
            x.hasBody = def.body.has_value() || (o && o->body);
            if (x.hasBody)
                x.body = o && o->body ? *o->body : *def.body;
            x.hasCollider = def.collider.has_value() || (o && o->collider);
            if (x.hasCollider)
                x.collider = o && o->collider ? *o->collider : *def.collider;
            x.hasAudioSource = def.audioSource.has_value() || (o && o->audioSource);
            if (x.hasAudioSource)
                x.audioSource = o && o->audioSource ? *o->audioSource : *def.audioSource;
            x.hasAudioListener = def.audioListener.has_value() || (o && o->audioListener);
            if (x.hasAudioListener)
                x.audioListener = o && o->audioListener ? *o->audioListener : *def.audioListener;
            x.hasAcoustics = def.acoustics.has_value() || (o && o->acoustics);
            if (x.hasAcoustics)
                x.acoustics = o && o->acoustics ? *o->acoustics : *def.acoustics;
            x.vfx = o && o->vfx ? *o->vfx : def.vfx;
            x.scripts = o && o->scripts ? *o->scripts : def.scripts;
            x.levelMesh = def.levelMesh;
            setLevelParts(x, def.level);
            x.prefabRoot = rootUid;
            x.prefabChild = def.id;
            prefabKeys[prefabKey(rootUid, def.id)] = x.uid;
        };
        std::unordered_map<std::string, uint32_t> uidOf;  // prefab id -> uid
        const std::string instanceName = e.name;
        const bool instanceHidden = e.hidden;
        fillFrom(prefab.entities[0], e);
        e.name = instanceName.empty() ? prefab.entities[0].name : instanceName;
        e.hidden = instanceHidden;
        uidOf[prefab.entities[0].id] = rootUid;
        out.push_back(e);
        std::unordered_set<std::string> removed;  // (deleted in the instance, and what hangs under them)
        for (size_t k = 1; k < prefab.entities.size(); ++k) {
            const PrefabEntity& def = prefab.entities[k];
            const PrefabOverride* o = overrideOf(def.id);
            if ((o && o->removed) || removed.count(def.parent)) {
                removed.insert(def.id);
                continue;
            }
            EntityEdit x;
            x.uid = nextExpanded++;
            fillFrom(def, x);
            x.name = o && o->name ? *o->name : def.name;
            x.hidden = o && o->hidden ? *o->hidden : def.hidden;
            const auto parent = uidOf.find(def.parent);
            x.parent = SceneEdits::createdKey(parent != uidOf.end() ? parent->second : rootUid);
            std::memcpy(x.world, o && o->hasLocal ? o->local : def.local, sizeof(x.world));
            uidOf[def.id] = x.uid;
            out.push_back(std::move(x));
        }
    }
    created = std::move(out);
}

std::vector<PrimitivePool> SceneEdits::reservePools(ScenePak& pak, uint32_t spare) const
{
    // Every cooked mesh can be created: the saved ones' slots, plus `spare` for the built-ins and
    // fewer for the rest (a scene can have thousands of meshes; past kPooledMeshes, none spare).
    constexpr uint32_t kPooledMeshes = 256, kOtherSpare = 16, kSkinnedSpare = 2;
    uint32_t skinCount = 0;
    const auto* skins = pak.view<fpak::MeshSkinGpu>(fpak::Section::MeshSkins, &skinCount);
    std::unordered_map<std::string, uint32_t> saved;
    for (const EntityEdit& e : created)
        if (e.kind == "primitive") {
            const std::string displaced = e.material.empty() ? std::string() : displacedMeshName(e.primitive, e.material);
            ++saved[!displaced.empty() && pak.findMesh(displaced) >= 0 ? displaced : e.primitive];
        }
    // A model (a library) can always be placed once more: its meshes get as many spare slots as
    // its nodes use them (a kitbashed scene repeats a mesh hundreds of times).
    std::vector<uint32_t> modelUses(pak.cookedMeshCount(), 0);
    if (spare != 0)
        for (const ScenePak::Library& lib : pak.libraries())
            for (const ScenePak::LibraryNode& n : lib.nodes)
                if (n.mesh < modelUses.size())
                    ++modelUses[n.mesh];
    std::vector<PrimitivePool> pools;
    uint32_t others = 0;
    for (uint32_t m = 0; m < pak.cookedMeshCount(); ++m) {
        const bool builtin = std::find(std::begin(pak.header().builtinMeshes), std::end(pak.header().builtinMeshes), m) !=
                             std::end(pak.header().builtinMeshes);
        PrimitivePool p;
        p.name = pak.meshName(m);
        p.mesh = m;
        uint32_t extra = spare == 0 ? 0u : builtin ? spare : others++ < kPooledMeshes ? std::min(spare, kOtherSpare) : 0u;
        // A skinned mesh's slot carries a skinning record (GPU vertices, a character): fewer spare.
        if (skins && m < skinCount && skins[m].firstSkinVertex != fpak::kNotSkinned)
            extra = std::min(extra, kSkinnedSpare);
        extra = std::max(extra, modelUses[m]);
        const auto it = saved.find(p.name);
        p.size = (it == saved.end() ? 0u : it->second) + extra;
        if (p.size > 0)
            p.first = pak.addInstancePool(p.mesh, p.size);
        pools.push_back(std::move(p));
    }
    return pools;
}

void SceneEdits::apply(ScenePak& pak, Scene& scene, std::vector<PrimitivePool>& pools, MaterialLibrary* materials, WaterSystem* water)
{
    auto& rows = pak.mutableInstances();
    const auto place = [&](uint32_t slot, const EntityEdit& e) {
        fpak::InstanceGpu& r = rows[slot];
        std::memcpy(r.world, e.world, sizeof(r.world));
        r.flags = (r.flags & ~uint32_t(fpak::kInstanceHidden)) | (e.hidden || e.deleted ? fpak::kInstanceHidden : 0u);
        if (const int32_t m = materials && !e.material.empty() ? materials->indexOf(e.material) : -1; m >= 0)
            r.flags = (r.flags & 0xffffu) | (uint32_t(m) + 1) << fpak::kInstanceMaterialShift;
    };
    for (EntityEdit& e : instances) {
        if (e.slot >= pak.cookedInstanceCount())
            continue;
        place(e.slot, e);
        if (flecs::entity ent = scene.entity(e.slot)) {
            WorldTransform t;
            std::memcpy(t.rows, e.world, sizeof(t.rows));
            ent.set<WorldTransform>(t);
            if (e.hasLight)  // (added in the editor's Inspector)
                ent.set<Light>(e.light);
            if (e.hasCamera)
                ent.set<CameraComponent>(e.camera);
            if (e.hasBody)
                ent.set<PhysicsBody>(e.body);
            if (e.hasCollider)
                ent.set<Collider>(e.collider);
            if (e.hasAudioSource)
                ent.set<AudioSource>(e.audioSource);
            if (e.hasAudioListener)
                ent.set<AudioListener>(e.audioListener);
            if (e.hasAcoustics)
                ent.set<AcousticSurface>(e.acoustics);
            if (!e.vfx.empty())
                setVfx(ent, e.vfx);
            setDataComponents(ent, e.data);
            if (e.deleted)
                ent.disable();
            e.entity = ent.id();
        }
    }
    std::unordered_map<uint32_t, flecs::entity> byUid;
    for (EntityEdit& e : created) {
        flecs::entity ent;
        if (e.kind == "primitive") {
            // (Its material's displaced copy of the mesh, when the scene has one.)
            const std::string displaced = e.material.empty() ? std::string() : displacedMeshName(e.primitive, e.material);
            auto pool = std::find_if(pools.begin(), pools.end(), [&](const PrimitivePool& p) { return p.name == displaced; });
            if (pool == pools.end() || pool->used >= pool->size)
                pool = std::find_if(pools.begin(), pools.end(), [&](const PrimitivePool& p) { return p.name == e.primitive; });
            if (pool == pools.end() || pool->used >= pool->size) {
                FX_WARN("edits: no mesh '%s' in the scene (skipped)", e.primitive.c_str());
                e.slot = ~0u;
                continue;
            }
            e.slot = pool->first + pool->used++;
            place(e.slot, e);
            ent = scene.spawnInstance(e.slot, rows[e.slot]);
        } else if (e.kind == "empty") {
            ent = scene.spawnTransform(e.world);
        } else if (e.kind == "level") {  // (LevelMeshes gives it its live mesh once the renderer is up)
            ent = scene.spawnTransform(e.world);
            ent.set<LevelMesh>({e.levelMesh ? e.levelMesh : std::make_shared<level::PolyMesh>()});
            if (e.levelMask)
                ent.set<LevelMask>({e.levelMask});
            if (!e.levelWear.empty())
                ent.set<LevelWear>({e.levelWear});
            if (!e.modifiers.empty())
                ent.set<LevelModifiers>({e.modifiers});
            if (e.spline)
                ent.set<LevelSpline>(*e.spline);
        } else if (e.kind == "light" && e.hasLight) {
            ent = scene.spawnTransform(e.world).set<Light>(e.light);
        } else if (e.kind == "camera" && e.hasCamera) {
            ent = scene.spawnTransform(e.world).set<CameraComponent>(e.camera);
        } else if (e.kind == "water" && e.hasWater) {
            ent = scene.spawnTransform(e.world);
            if (water)
                water->add(e.water, ent.id());
        } else {
            FX_WARN("edits: unknown entity kind '%s' (skipped)", e.kind.c_str());
            continue;
        }
        // A Light or camera on any other kind: added in the Inspector (or by a prefab's override).
        if (e.hasLight && !ent.has<Light>())
            ent.set<Light>(e.light);
        if (e.hasCamera && !ent.has<CameraComponent>())
            ent.set<CameraComponent>(e.camera);
        if (e.hasBody)
            ent.set<PhysicsBody>(e.body);
        if (e.hasCollider)
            ent.set<Collider>(e.collider);
        if (e.hasAudioSource)
            ent.set<AudioSource>(e.audioSource);
        if (e.hasAudioListener)
            ent.set<AudioListener>(e.audioListener);
        if (e.hasAcoustics)
            ent.set<AcousticSurface>(e.acoustics);
        if (e.cut)
            ent.set<CutVolume>(*e.cut);
        if (e.scatterSet)
            ent.set<ScatterSet>(*e.scatterSet);
        if (e.snapPoint)
            ent.set<SnapPoint>(*e.snapPoint);
        if (e.volume)
            ent.set<LevelVolume>(*e.volume);
        if (e.wall)
            ent.set<LevelWall>({*e.wall});
        if (e.roof)
            ent.set<LevelRoof>({*e.roof});
        if (e.opening)
            ent.set<WallOpening>(*e.opening);
        if (!e.vfx.empty())
            setVfx(ent, e.vfx);
        setDataComponents(ent, e.data);
        e.entity = ent.id();
        byUid[e.uid] = ent;
    }

    for (EntityEdit& e : lights) {
        flecs::entity ent = scene.lightEntity(e.slot);
        if (!ent) {
            FX_WARN("edits: no cooked light %u (skipped)", e.slot);
            continue;
        }
        WorldTransform t;
        std::memcpy(t.rows, e.world, sizeof(t.rows));
        ent.set<WorldTransform>(t);
        if (e.hasLight)
            ent.set<Light>(e.light);
        if (e.hasCamera)  // (added in the Inspector)
            ent.set<CameraComponent>(e.camera);
        if (e.hasBody)
            ent.set<PhysicsBody>(e.body);
        if (e.hasCollider)
            ent.set<Collider>(e.collider);
        if (e.hasAudioSource)
            ent.set<AudioSource>(e.audioSource);
        if (e.hasAudioListener)
            ent.set<AudioListener>(e.audioListener);
        if (e.hasAcoustics)
            ent.set<AcousticSurface>(e.acoustics);
        if (!e.vfx.empty())
            setVfx(ent, e.vfx);
        setDataComponents(ent, e.data);
        if (e.deleted)
            ent.disable();
        e.entity = ent.id();
    }

    // Parents, once every entity exists (world transforms are kept).
    const auto byKey = [&](const std::string& key) {
        if (key.size() < 2)
            return flecs::entity();
        const uint32_t n = uint32_t(std::strtoul(key.c_str() + 1, nullptr, 10));
        if (key[0] == 'i')
            return n < pak.cookedInstanceCount() ? scene.entity(n) : flecs::entity();
        if (key[0] == 'l')
            return scene.lightEntity(n);
        if (key[0] == 'p') {  // an entity of a prefab instance
            const auto expanded = prefabKeys.find(key);
            const auto it = expanded == prefabKeys.end() ? byUid.end() : byUid.find(expanded->second);
            return it != byUid.end() ? it->second : flecs::entity();
        }
        auto it = byUid.find(n);
        return key[0] == 'c' && it != byUid.end() ? it->second : flecs::entity();
    };
    std::unordered_map<flecs::entity_t, bool> hidden;
    for (std::vector<EntityEdit>* list : {&instances, &created, &lights})
        for (const EntityEdit& e : *list) {
            if (!e.entity)
                continue;
            hidden[e.entity] = e.hidden;
            if (e.parent.empty())
                continue;
            if (e.parent[0] == 'b')
                continue;  // under a bone: expandBones(), once the skeletons are there
            if (const flecs::entity parent = byKey(e.parent); parent && parent.id() != e.entity)
                scene.setLocal(flecs::entity(scene.world(), e.entity), parent, e.world);
            else
                FX_WARN("edits: parent '%s' not found (kept at the root)", e.parent.c_str());
        }
    // Children's world transforms, and their rows (physics and the upload read those).
    scene.propagate();
    for (std::vector<EntityEdit>* list : {&instances, &created, &lights})
        for (const EntityEdit& e : *list)
            if (e.entity && e.slot != ~0u && !e.parent.empty())
                if (const WorldTransform* t = flecs::entity(scene.world(), e.entity).try_get<WorldTransform>())
                    std::memcpy(rows[e.slot].world, t->rows, sizeof(rows[e.slot].world));
    // A hidden entity hides everything below it: mesh rows, and lights (a Light flag).
    for (std::vector<EntityEdit>* list : {&instances, &created, &lights})
        for (const EntityEdit& e : *list) {
            if (!e.entity)
                continue;
            const flecs::entity ent(scene.world(), e.entity);
            bool hide = e.hidden;
            uint32_t depth = 0;
            for (flecs::entity p = scene.parentOf(ent); p && depth < 256 && !hide; p = scene.parentOf(p), ++depth)
                hide = hidden.count(p.id()) && hidden[p.id()];
            if (const Light* l = ent.try_get<Light>(); l && hide) {
                Light flagged = *l;
                flagged.flags |= 2u;
                ent.set<Light>(flagged);
            }
            if (hide && list != &lights && e.slot != ~0u && ent.has<MeshInstance>())
                rows[e.slot].flags |= fpak::kInstanceHidden;
            if (const int32_t body = hide && water ? water->find(e.entity) : -1; body >= 0)
                water->setHidden(uint32_t(body), true);
        }
}

}  // namespace fx

namespace fx {

flecs::entity_t SceneEdits::entityOfKey(const ScenePak& pak, Scene& scene, const std::string& key) const
{
    if (key.size() < 2)
        return 0;
    const uint32_t n = uint32_t(std::strtoul(key.c_str() + 1, nullptr, 10));
    if (key[0] == 'i')
        return n < pak.cookedInstanceCount() ? scene.entity(n).id() : 0;
    if (key[0] == 'l')
        return scene.lightEntity(n).id();
    uint32_t uid = n;
    if (key[0] == 'p') {
        const auto it = prefabKeys.find(key);
        if (it == prefabKeys.end())
            return 0;
        uid = it->second;
    } else if (key[0] != 'c') {
        return 0;
    }
    for (const EntityEdit& e : created)
        if (e.uid == uid)
            return e.entity;
    return 0;
}

uint32_t SceneEdits::expandBones(const ScenePak& pak, Scene& scene, BoneEntities& bones, const AnimationSystem& animation,
                                 const SkinningSystem& skinning)
{
    uint32_t expanded = 0;
    for (std::vector<EntityEdit>* list : {&instances, &created})
        for (const EntityEdit& e : *list)
            if (e.bones && e.entity && !e.deleted) {
                if (bones.expand(scene, flecs::entity(scene.world(), e.entity), animation, skinning))
                    ++expanded;
                else
                    FX_WARN("edits: '%s' has no skinned mesh: its bones are not expanded", e.name.c_str());
            }
    // What hangs under a bone: "b<root key>#<bone>".
    bool linked = false;
    for (std::vector<EntityEdit>* list : {&instances, &created, &lights})
        for (const EntityEdit& e : *list) {
            if (!e.entity || e.parent.size() < 2 || e.parent[0] != 'b')
                continue;
            const size_t hash = e.parent.find('#');
            const flecs::entity_t root = hash == std::string::npos ? 0 : entityOfKey(pak, scene, e.parent.substr(1, hash - 1));
            const flecs::entity_t bone = root ? bones.bone(root, std::string_view(e.parent).substr(hash + 1)) : 0;
            if (bone && bone != e.entity) {
                scene.setLocal(flecs::entity(scene.world(), e.entity), flecs::entity(scene.world(), bone), e.world);
                linked = true;
            } else {
                FX_WARN("edits: parent '%s' not found (kept at the root)", e.parent.c_str());
            }
        }
    if (linked)
        scene.propagate();
    return expanded;
}

}  // namespace fx
