#include "script/script_host.h"

#include "assets/scene_pak.h"
#include "assets/sound_asset.h"
#include "assets/surface_sound.h"
#include "audio/audio_system.h"
#include "core/dmath.h"
#include "core/json.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/system_stats.h"
#include "anim/animation_system.h"
#include "physics/physics_world.h"
#include "editor/prefab_spawn.h"
#include "render/skinning.h"
#include "vfx/particle_system.h"
#include "scene/scene.h"
#include "ui/ui_layer.h"
#include "render/gpu_driven_renderer.h"
#include "vfx/runtime_meshes.h"
#include "editor/material_library.h"
#include "project/project_shaders.h"
#include "render/mesh_renderer.h"
#include "script/script_level.h"
#include "level/modifiers.h"
#include "render/shader_file.h"
#include "terrain/terrain.h"
#include "terrain/terrain_layers.h"
#include "steam/steam_client.h"
#include "vfx/vfx_components.h"
#include "core/platform.h"
#include "water/water_system.h"

#include <SDL3/SDL.h>
#include <flecs.h>

#define NETHOST_USE_AS_STATIC
#include <coreclr_delegates.h>
#include <hostfxr.h>
#include <nethost.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

namespace fx {

namespace {

using Context = ScriptHost::Context;

Context& ctxOf(void* c) { return *static_cast<Context*>(c); }
ecs_world_t* worldOf(void* c) { return ctxOf(c).scene->world().c_ptr(); }

// ---- native API (called from managed code)

void apiLog(void*, int32_t level, const char* message)
{
    if (level >= 2)
        FX_ERROR("script: %s", message);
    else if (level == 1)
        FX_WARN("script: %s", message);
    else
        FX_INFO("script: %s", message);
}

uint64_t apiComponentId(void* c, const char* name, uint32_t size, uint32_t align)
{
    ecs_world_t* w = worldOf(c);
    if (ecs_entity_t e = ecs_lookup_path_w_sep(w, 0, name, "::", "::", false)) {
        const ecs_type_info_t* ti = ecs_get_type_info(w, e);
        if (!ti || uint32_t(ti->size) != size || uint32_t(ti->alignment) != align) {
            FX_ERROR("script: component %s is %d bytes (align %d) natively but %u (align %u) in C#", name,
                     ti ? ti->size : 0, ti ? ti->alignment : 0, size, align);
            return 0;
        }
        return e;
    }
    // A script-defined component: registered with the managed layout (plain data, no hooks).
    ecs_entity_desc_t ed = {};
    ed.name = name;
    ed.sep = "::";  // C# full names use '.', so they stay one path element
    ecs_component_desc_t cd = {};
    cd.entity = ecs_entity_init(w, &ed);
    cd.type.size = int32_t(size);
    cd.type.alignment = int32_t(align);
    const ecs_entity_t id = ecs_component_init(w, &cd);
    ctxOf(c).components.insert(id);
    return id;
}

void trackCreated(Context& x, uint64_t e)
{
    if (x.tracking && e && x.createdSet.insert(e).second)
        x.created.push_back(e);
}

uint64_t apiEntityCreate(void* c)
{
    const uint64_t e = ecs_new(worldOf(c));
    trackCreated(ctxOf(c), e);
    return e;
}

// ---- (v18) rows the game changes: an instance's visibility, shadows, tint and material

// The instance row an entity draws with (not a spawned body's: physics streams those), or ~0u.
uint32_t rowOf(Context& x, uint64_t entity)
{
    ecs_world_t* w = worldOf(&x);
    if (!x.pak || !ecs_is_alive(w, entity))
        return ~0u;
    const auto* mi = static_cast<const MeshInstance*>(ecs_get_id(w, entity, x.idMeshInstance));
    return mi && !ecs_has_id(w, entity, x.idRigidBody) && mi->gpuSlot < x.pak->instances().size() ? mi->gpuSlot : ~0u;
}

// A spawned mesh's row (RuntimeMeshes shows and hides it, from its entities' being enabled).
bool spawnedRow(const Context& x, uint32_t slot) { return x.runtimeMeshes && x.runtimeMeshes->owns(slot); }

// The row as the game changes it, kept as it was the first time (Stop puts that back).
Context::Row& rowState(Context& x, uint32_t slot)
{
    const auto [it, added] = x.rows.try_emplace(slot);
    if (added)
        it->second.original = x.pak->instances()[slot];
    return it->second;
}

// Why a row is hidden changed (bit: 1 disabled or destroyed, 2 Renderer.Visible): flagged so.
void hideRow(Context& x, uint32_t slot, uint8_t why, bool on)
{
    if (slot == ~0u || spawnedRow(x, slot))
        return;
    Context::Row& r = rowState(x, slot);
    r.hide = uint8_t(on ? r.hide | why : r.hide & ~why);
    const bool hidden = (r.original.flags & fpak::kInstanceHidden) || r.hide;
    fpak::InstanceGpu& row = x.pak->mutableInstances()[slot];
    row.flags = (row.flags & ~uint32_t(fpak::kInstanceHidden)) | (hidden ? fpak::kInstanceHidden : 0u);
    x.touched.push_back(slot);
}

// An entity disabled or enabled (Enabled, or destroyed while the editor plays): its row and its
// spawned body with it. (Bodies of the scene's entities follow by themselves: PhysicsWorld::step.)
void setEntityEnabled(Context& x, uint64_t entity, bool enabled)
{
    ecs_enable(worldOf(&x), entity, enabled);
    hideRow(x, rowOf(x, entity), 1, !enabled);
    if (x.physics)
        x.physics->setBodyEnabled(entity, enabled);
}

// The entity and everything below it (Parent), the entity first.
std::vector<flecs::entity_t> subtreeOf(Context& x, uint64_t root)
{
    std::vector<flecs::entity_t> all = {root};
    x.scene->descendantsOf(root, all);
    return all;
}

// Destroys the entity and everything below it. While the editor plays, entities of the scene are
// disabled instead (Stop brings them back); the game's own, and a game's, are deleted.
void apiEntityDestroy(void* c, uint64_t e)
{
    Context& x = ctxOf(c);
    ecs_world_t* w = worldOf(c);
    if (!ecs_is_alive(w, e))
        return;
    const std::vector<flecs::entity_t> all = subtreeOf(x, e);
    for (auto it = all.rbegin(); it != all.rend(); ++it) {  // (children first)
        const uint64_t id = *it;
        if (!ecs_is_alive(w, id))
            continue;
        if (x.tracking && !x.createdSet.count(id)) {
            if (!ecs_has_id(w, id, EcsDisabled))
                setEntityEnabled(x, id, false);
            else if (!x.scriptDisabled.erase(id))  // (destroyed: Enabled can't bring it back)
                continue;                          // (deleted in the editor)
            x.disabled.push_back(id);
            continue;
        }
        if (const uint32_t slot = rowOf(x, id); slot != ~0u && !spawnedRow(x, slot)) {
            hideRow(x, slot, 1, true);  // (the slot stays hidden: nothing draws it again)
            x.scene->forgetInstance(slot);
        }
        if (x.physics)
            x.physics->removeBody(id);
        ecs_delete(w, id);
    }
}
int32_t apiEntityAlive(void* c, uint64_t e) { return ecs_is_alive(worldOf(c), e) ? 1 : 0; }
void apiEntitySet(void* c, uint64_t e, uint64_t comp, const void* data, uint32_t size)
{
    ecs_set_id(worldOf(c), e, comp, size, data);
}
const void* apiEntityGet(void* c, uint64_t e, uint64_t comp) { return ecs_get_id(worldOf(c), e, comp); }
void apiEntityRemove(void* c, uint64_t e, uint64_t comp) { ecs_remove_id(worldOf(c), e, comp); }

struct ScriptQuery {
    ecs_query_t* query = nullptr;
    ecs_iter_t it = {};
    bool iterating = false;
    uint32_t count = 0;
    uint32_t sizes[FLECS_TERM_COUNT_MAX] = {};
    uint32_t access[FLECS_TERM_COUNT_MAX] = {};
};

void* apiQueryCreate(void* c, const uint64_t* components, const uint32_t* access, uint32_t count)
{
    ecs_world_t* w = worldOf(c);
    if (count == 0 || count > FLECS_TERM_COUNT_MAX)
        return nullptr;
    ecs_query_desc_t desc = {};
    auto* q = new ScriptQuery();
    q->count = count;
    for (uint32_t i = 0; i < count; ++i) {
        desc.terms[i].id = components[i];
        q->access[i] = access[i];
        if (access[i] == kFxAccessNot) {
            desc.terms[i].oper = EcsNot;
            desc.terms[i].inout = EcsInOutNone;
        } else {
            desc.terms[i].inout = int16_t(access[i] == kFxAccessIn ? EcsIn : EcsInOut);
            const ecs_type_info_t* ti = ecs_get_type_info(w, components[i]);
            q->sizes[i] = ti ? uint32_t(ti->size) : 0;
        }
    }
    desc.cache_kind = EcsQueryCacheAuto;
    q->query = ecs_query_init(w, &desc);
    if (!q->query) {
        delete q;
        return nullptr;
    }
    return q;
}

void apiQueryEnd(void*, void* query)
{
    auto* q = static_cast<ScriptQuery*>(query);
    if (!q->iterating)
        return;
    // Advancing marks the current table's written columns changed (flecs does it on next).
    if (ecs_query_next(&q->it))
        ecs_iter_fini(&q->it);
    q->iterating = false;
}

void apiQueryDestroy(void* c, void* query)
{
    auto* q = static_cast<ScriptQuery*>(query);
    if (!q)
        return;
    apiQueryEnd(c, q);
    ecs_query_fini(q->query);
    delete q;
}

int32_t apiQueryNext(void* c, void* query, void** columns, const uint64_t** entities)
{
    auto* q = static_cast<ScriptQuery*>(query);
    if (!q->iterating) {
        q->it = ecs_query_iter(worldOf(c), q->query);
        q->iterating = true;
    }
    while (ecs_query_next(&q->it)) {
        if (q->it.count == 0)
            continue;
        for (uint32_t i = 0; i < q->count; ++i)
            columns[i] = q->access[i] == kFxAccessNot ? nullptr : ecs_field_w_size(&q->it, q->sizes[i], int8_t(i));
        *entities = q->it.entities;
        return q->it.count;
    }
    q->iterating = false;  // ecs_query_next finished the iterator
    return 0;
}

void apiMeshBounds(void* c, uint32_t mesh, float* center, float* radius)
{
    uint32_t count = 0;
    const auto* meshes = ctxOf(c).pak->view<fpak::MeshGpu>(fpak::Section::Meshes, &count);
    if (mesh >= count) {
        center[0] = center[1] = center[2] = *radius = 0.0f;
        return;
    }
    const fpak::MeshGpu& m = meshes[mesh];
    float r2 = 0.0f;
    for (int a = 0; a < 3; ++a) {
        const float half = m.posScale[a] * 65535.0f * 0.5f;
        center[a] = m.posOffset[a] + half;
        r2 += half * half;
    }
    *radius = std::sqrt(r2);
}

void apiCameraGet(void* c, float* position, float* forward, float* right, float* up)
{
    const CameraState& cam = ctxOf(c).camera;
    const float3 v[4] = {cam.position, cam.forward, cam.right, cam.up};
    float* out[4] = {position, forward, right, up};
    for (int i = 0; i < 4; ++i)
        if (out[i])
            std::memcpy(out[i], &v[i], 12);
}

uint64_t apiBodySpawn(void* c, uint32_t shape, const float* position, const float* size, const float* velocity,
                      uint32_t tint)
{
    PhysicsWorld* physics = ctxOf(c).physics;
    if (!physics)
        return 0;
    auto f3 = [](const float* p) { return p ? float3{p[0], p[1], p[2]} : float3{0, 0, 0}; };
    const uint64_t e = physics->spawn(shape == 0 ? PhysicsWorld::Shape::Box : PhysicsWorld::Shape::Sphere, f3(position), f3(size),
                                      f3(velocity), {0, 0, 0}, tint);
    trackCreated(ctxOf(c), e);  // (Stop removes it with the game's entities)
    return e;
}

bool bodyOf(void* c, uint64_t entity, b3BodyId& id)
{
    Context& x = ctxOf(c);
    ecs_world_t* w = worldOf(c);
    if (!ecs_is_alive(w, entity))
        return false;
    // A spawned body, or a scene entity's PhysicsBody while the game runs; else its Collider's (a
    // static body, through the physics world).
    if (const auto* rb = static_cast<const RigidBody*>(ecs_get_id(w, entity, x.idRigidBody)))
        std::memcpy(&id, rb->bodyId, sizeof(id));
    else if (const auto* pb = static_cast<const PhysicsBody*>(ecs_get_id(w, entity, x.idPhysicsBody)))
        std::memcpy(&id, pb->bodyId, sizeof(id));
    else if (x.physics)
        id = x.physics->bodyOfEntity(entity);
    else
        return false;
    return b3Body_IsValid(id);
}

void apiBodyImpulse(void* c, uint64_t entity, const float* impulse)
{
    b3BodyId id;
    if (bodyOf(c, entity, id))
        b3Body_ApplyLinearImpulseToCenter(id, {impulse[0], impulse[1], impulse[2]}, true);
}

void apiBodyVelocity(void* c, uint64_t entity, float* velocity)
{
    b3BodyId id;
    const b3Vec3 v = bodyOf(c, entity, id) ? b3Body_GetLinearVelocity(id) : b3Vec3{0, 0, 0};
    velocity[0] = v.x, velocity[1] = v.y, velocity[2] = v.z;
}

int32_t apiWaterSample(void* c, const float* position, FxWaterSample* out)
{
    const WaterSystem* water = static_cast<ScriptHost::Context*>(c)->water;
    const WaterSample s = water ? water->sample({position[0], position[1], position[2]}) : WaterSample{};
    *out = {s.body, s.height, {s.normal.x, s.normal.y, s.normal.z}, {s.flow.x, s.flow.y, s.flow.z}, s.depth, 0};
    return s.body >= 0 ? 1 : 0;
}

void apiWaterSplash(void* c, const float* position, float radius, float strength)
{
    if (const WaterSystem* water = static_cast<ScriptHost::Context*>(c)->water)
        water->splash(position[0], position[2], std::max(radius, 0.01f), strength);
}

// ---- audio (v7)

uint32_t apiAudioSound(void*, const char* path) { return path ? soundId(path) : 0; }

uint32_t apiAudioPlay(void* c, uint32_t sound, const float* position, uint64_t entity, float volume, float pitch, uint32_t flags)
{
    AudioSystem* audio = ctxOf(c).audio;
    if (!audio)
        return 0;
    const float3 at = position ? float3{position[0], position[1], position[2]} : float3{};
    return audio->playScript(sound, position ? &at : nullptr, entity, volume, pitch, (flags & 1u) != 0);
}

uint32_t apiAudioImpact(void* c, uint32_t surface, const float* position, float volume, float pitch)
{
    AudioSystem* audio = ctxOf(c).audio;
    return audio && position ? audio->playScriptImpact(surface, {position[0], position[1], position[2]}, volume, pitch) : 0;
}

void apiAudioStop(void* c, uint32_t handle, float fade)
{
    if (AudioSystem* audio = ctxOf(c).audio; audio && handle)
        audio->stop(handle, std::max(fade, 0.0f));
}

void apiAudioSet(void* c, uint32_t handle, float volume, float pitch, const float* position)
{
    if (AudioSystem* audio = ctxOf(c).audio; audio && handle) {
        const float3 at = position ? float3{position[0], position[1], position[2]} : float3{};
        audio->setScriptVoice(handle, volume, pitch, position ? &at : nullptr);
    }
}

int32_t apiAudioPlaying(void* c, uint32_t handle)
{
    const AudioSystem* audio = ctxOf(c).audio;
    return audio && handle && audio->active() && audio->playing(handle) ? 1 : 0;
}

// ---- (v35) audio streams

// Destroyed streams freed once the mixer is done with them (all of them without audio).
void reapStreams(Context& x)
{
    std::erase_if(x.dyingStreams, [&](const Context::Stream& s) {
        return !x.audio || !x.audio->active() || !s.voice || !x.audio->playing(s.voice);
    });
}

void dropStream(Context& x, uint32_t id)
{
    const auto it = x.streams.find(id);
    if (it == x.streams.end())
        return;
    if (x.audio && it->second.voice)
        x.audio->stop(it->second.voice, 0.0f);
    x.dyingStreams.push_back(std::move(it->second));
    x.streams.erase(it);
    reapStreams(x);
}

uint32_t apiStreamCreate(void* c, uint32_t channels, float seconds)
{
    Context& x = ctxOf(c);
    if (!x.audio || !x.audio->active())
        return 0;
    const uint32_t frames = uint32_t(std::clamp(seconds, 0.02f, 30.0f) * float(kAudioSampleRate));
    const uint32_t id = x.nextStream++;
    x.streams[id].stream = std::make_unique<AudioStream>(channels, frames);
    return id;
}

uint32_t apiStreamWrite(void* c, uint32_t stream, const float* samples, uint32_t frames)
{
    Context& x = ctxOf(c);
    const auto it = x.streams.find(stream);
    return it != x.streams.end() && samples ? it->second.stream->write(samples, frames) : 0;
}

void apiStreamInfo(void* c, uint32_t stream, uint32_t* queued, uint32_t* free, uint32_t* underruns)
{
    Context& x = ctxOf(c);
    const auto it = x.streams.find(stream);
    const AudioStream* s = it != x.streams.end() ? it->second.stream.get() : nullptr;
    const uint32_t q = s ? s->queued() : 0;
    if (queued)
        *queued = q;
    if (free)
        *free = s ? s->capacity() - q : 0;
    if (underruns)
        *underruns = s ? s->underruns() : 0;
}

uint32_t apiStreamPlay(void* c, uint32_t stream, const float* position, uint64_t entity, float volume, uint32_t bus)
{
    Context& x = ctxOf(c);
    const auto it = x.streams.find(stream);
    if (!x.audio || it == x.streams.end())
        return 0;
    if (it->second.voice)  // (one voice at a time: the one before stops)
        x.audio->stop(it->second.voice, 0.0f);
    const float3 at = position ? float3{position[0], position[1], position[2]} : float3{};
    it->second.voice = x.audio->playScriptStream(it->second.stream.get(), position ? &at : nullptr, entity, volume, bus);
    return it->second.voice;
}

void apiStreamDestroy(void* c, uint32_t stream) { dropStream(ctxOf(c), stream); }

// ---- (v35) collision shapes from scripts

int32_t apiPhysicsShapeAdd(void* c, uint64_t entity, const FxShape* shape, const float* points, int32_t pointCount)
{
    Context& x = ctxOf(c);
    if (!x.physics || !shape || !entity)
        return 0;
    PhysicsWorld::ScriptShape s;
    s.kind = shape->kind;
    std::memcpy(s.center, shape->center, sizeof(s.center));
    std::memcpy(s.rotation, shape->rotation, sizeof(s.rotation));
    std::memcpy(s.size, shape->size, sizeof(s.size));
    if (points && pointCount > 0)
        s.points.assign(points, points + size_t(std::min(pointCount, 4096)) * 3);
    return x.physics->addScriptShape(entity, std::move(s)) ? 1 : 0;
}

void apiPhysicsShapesClear(void* c, uint64_t entity)
{
    if (PhysicsWorld* physics = ctxOf(c).physics)
        physics->clearScriptShapes(entity);
}

// (v38) Network transports.
ScriptHost::Context::Transport* transportOf(void* c, uint32_t handle)
{
    ScriptHost::Context& x = ctxOf(c);
    return handle && handle <= x.transports.size() && x.transports[handle - 1].transport ? &x.transports[handle - 1] : nullptr;
}

uint32_t apiNetOpen(void* c, const char* address)
{
    ScriptHost::Context& x = ctxOf(c);
    std::unique_ptr<net::Transport> t = net::makeTransport(address ? address : "");
    if (!t)
        return 0;
    t = net::makeConditioner(std::move(t), x.netConditions);
    for (size_t i = 0; i < x.transports.size(); ++i)
        if (!x.transports[i].transport) {
            x.transports[i] = {std::move(t)};
            return uint32_t(i + 1);
        }
    x.transports.push_back({std::move(t)});
    return uint32_t(x.transports.size());
}

void apiNetClose(void* c, uint32_t handle)
{
    if (ScriptHost::Context::Transport* t = transportOf(c, handle))
        *t = {};
}

int32_t apiNetListen(void* c, uint32_t handle, int32_t port)
{
    ScriptHost::Context::Transport* t = transportOf(c, handle);
    if (!t || port < 0 || port > 65535 || !t->transport->listen(uint16_t(port)))
        return -1;
    return t->transport->boundPort();
}

uint32_t apiNetConnect(void* c, uint32_t handle, const char* address)
{
    ScriptHost::Context::Transport* t = transportOf(c, handle);
    return t && address ? t->transport->connect(address) : 0;
}

int32_t apiNetSend(void* c, uint32_t handle, uint32_t connection, int32_t channel, const uint8_t* data, int32_t size)
{
    ScriptHost::Context::Transport* t = transportOf(c, handle);
    if (!t || size < 0 || (size && !data))
        return 0;
    const std::span<const uint8_t> bytes(data, size_t(size));
    return (channel < 0 ? t->transport->sendDatagram(connection, bytes) : t->transport->send(connection, uint8_t(channel), bytes)) ? 1 : 0;
}

void apiNetDisconnect(void* c, uint32_t handle, uint32_t connection, const char* reason)
{
    if (ScriptHost::Context::Transport* t = transportOf(c, handle))
        t->transport->disconnect(connection, reason ? reason : "closed");
}

int32_t apiNetPoll(void* c, uint32_t handle, FxNetEvent* event, uint8_t* buffer, int32_t capacity)
{
    ScriptHost::Context::Transport* t = transportOf(c, handle);
    if (!t || !event)
        return 0;
    if (!t->waiting) {
        if (!t->transport->poll(t->event))
            return 0;
        t->waiting = true;
        if (t->event.type == net::Event::Type::Disconnected)
            t->event.data.assign(t->event.text.begin(), t->event.text.end());
    }
    const net::Event& e = t->event;
    event->type = e.type == net::Event::Type::Connected      ? 1
                  : e.type == net::Event::Type::Disconnected ? 2
                  : e.type == net::Event::Type::Message      ? 3
                                                             : 4;
    event->connection = e.connection;
    event->channel = e.type == net::Event::Type::Datagram ? -1 : int32_t(e.channel);
    event->size = int32_t(e.data.size());
    if (int32_t(e.data.size()) > capacity || (!e.data.empty() && !buffer))
        return -1;  // (kept: polled again with a bigger buffer)
    if (!e.data.empty())
        std::memcpy(buffer, e.data.data(), e.data.size());
    t->waiting = false;
    return 1;
}

void apiNetStats(void* c, uint32_t handle, uint32_t connection, FxNetStats* out)
{
    if (!out)
        return;
    *out = {};
    if (ScriptHost::Context::Transport* t = transportOf(c, handle)) {
        const net::ConnectionStats s = t->transport->stats(connection);
        *out = {s.rttMs, s.lossPercent, s.bytesSent, s.bytesReceived, s.maxDatagram, 0};
    }
}

int32_t apiNetCertificate(void* c, uint32_t handle, char* buffer, int32_t capacity)
{
    ScriptHost::Context::Transport* t = transportOf(c, handle);
    const std::string s = t ? t->transport->certificate() : std::string();
    if (int32_t(s.size()) > capacity || (!s.empty() && !buffer))
        return -int32_t(s.size());
    if (!s.empty())
        std::memcpy(buffer, s.data(), s.size());
    return int32_t(s.size());
}

int32_t apiNetLaunch(void* c, char* buffer, int32_t capacity)
{
    const std::string& s = ctxOf(c).netLaunch;
    if (int32_t(s.size()) > capacity || !buffer)
        return -int32_t(s.size());
    std::memcpy(buffer, s.data(), s.size());
    return int32_t(s.size());
}

void apiNetLaunchClients(void* c, int32_t count, const char* address)
{
    ScriptHost::Context& x = ctxOf(c);
    if (x.launchClients && count > 0 && address)
        x.launchClients(count, address);
}

void apiSimSetTick(void* c, uint64_t tick) { ctxOf(c).nextTick = tick; }
void apiSimSetRate(void* c, float rate) { ctxOf(c).simRate = std::clamp(rate, 0.0f, 2.0f); }  // (0: no ticks: a rollback session waiting for inputs)
int32_t apiServerCommand(void* c, char* buffer, int32_t capacity)
{
    ScriptHost::Context& x = ctxOf(c);
    static thread_local std::string waiting;  // (a line too long for the buffer, kept)
    if (waiting.empty() && (!x.serverCommand || !x.serverCommand(waiting)))
        return 0;
    if (int32_t(waiting.size()) > capacity || !buffer)
        return -int32_t(waiting.size());
    std::memcpy(buffer, waiting.data(), waiting.size());
    const int32_t n = int32_t(waiting.size());
    waiting.clear();
    return n;
}

void* apiSteamProc(void*, const char* name)
{
    return steam::proc(name);
}

int32_t apiSteamStatus(void*, uint32_t* appId, char* error, int32_t capacity)
{
    const steam::Status& s = steam::status();
    if (appId)
        *appId = s.appId;
    if (error && capacity > 0) {  // (cut to fit, ended with a NUL)
        const size_t n = std::min(s.error.size(), size_t(capacity - 1));
        std::memcpy(error, s.error.data(), n);
        error[n] = 0;
    }
    return int32_t(s.state);
}

void apiSimMaxTicks(void* c, uint32_t ticks) { ctxOf(c).maxTicks = ticks; }

void apiSimWindow(void* c, uint32_t ticks)
{
    ScriptHost::Context& x = ctxOf(c);
    if (x.rollbackWindow)
        x.rollbackWindow(ticks);
}

int32_t apiSimRollback(void* c, uint64_t ticksBack)
{
    ScriptHost::Context& x = ctxOf(c);
    return x.rollback && x.rollback(ticksBack) ? 1 : 0;
}

uint64_t apiSimPhysicsHash(void* c, uint64_t ticksBack)
{
    ScriptHost::Context& x = ctxOf(c);
    return x.physicsHash ? x.physicsHash(ticksBack) : 0;
}

void apiSteamPump(void*)
{
    steam::pump();
}

int32_t apiSteamNextCallback(void*, int32_t* id, void* buffer, int32_t capacity)
{
    static thread_local steam::Callback waiting;  // (one too big for the buffer, kept)
    static thread_local bool held = false;
    if (!held && !steam::nextCallback(waiting))
        return 0;
    held = true;
    if (id)
        *id = waiting.id;
    const int32_t size = int32_t(waiting.data.size());
    if (size > capacity || (!buffer && size > 0))
        return -size;
    if (size > 0)
        std::memcpy(buffer, waiting.data.data(), size);
    held = false;
    return size > 0 ? size : (waiting.id != 0 ? 1 : 0);  // (an empty callback still counts)
}

void apiSimFocus(void* c, const float* points, int32_t count)
{
    std::vector<float3>& focus = ctxOf(c).simFocus;
    focus.clear();
    for (int32_t i = 0; points && i < std::min(count, 4096); ++i)
        focus.push_back({points[i * 3], points[i * 3 + 1], points[i * 3 + 2]});
}

void apiAudioBusVolume(void* c, uint32_t bus, float volume)
{
    if (AudioSystem* audio = ctxOf(c).audio)
        audio->setScriptBusVolume(bus, volume);
}

// A contact side's surface (a preset's 1-based index, 0: its own) and entity (0: the scene's).
uint32_t surfaceIndexOf(void* c, const ContactSide& side, float3 point)
{
    AudioSystem* audio = ctxOf(c).audio;
    if (!audio || !audio->active())
        return 0;
    std::string preset;
    audio->surfaceOf(side, point, &preset);
    return surfacePresetIndex(preset);
}
uint64_t entityOf(const ContactSide& side) { return side.kind == ContactOwner::Entity ? side.id : 0; }

int32_t apiContactPoll(void* c, FxContact* out, int32_t capacity)
{
    const PhysicsWorld* physics = ctxOf(c).physics;
    if (!physics)
        return 0;
    const std::vector<PhysicsImpact>& hits = physics->impacts();
    const size_t from = ctxOf(c).stepEvents ? std::min(physics->stepImpactsBegin(), hits.size()) : 0;  // (phase 6: the step's)
    for (int32_t i = 0; out && i < capacity && from + size_t(i) < hits.size(); ++i) {
        const PhysicsImpact& h = hits[from + size_t(i)];
        out[i] = {entityOf(h.a), entityOf(h.b), {h.point.x, h.point.y, h.point.z}, {h.normal.x, h.normal.y, h.normal.z}, h.speed,
                  surfaceIndexOf(c, h.a, h.point), surfaceIndexOf(c, h.b, h.point), 0};
    }
    return int32_t(hits.size() - from);
}

int32_t apiSurfaceAt(void* c, const float* position, float maxDistance, FxSurfaceHit* out)
{
    const PhysicsWorld* physics = ctxOf(c).physics;
    ContactSide side;
    float3 point, normal;
    float fraction = 0.0f;
    const float reach = std::max(maxDistance, 0.0f);
    if (!physics || !position || reach <= 0.0f ||
        !physics->castRay({position[0], position[1], position[2]}, {0.0f, -reach, 0.0f}, side, point, normal, fraction))
        return 0;
    *out = {{point.x, point.y, point.z}, {normal.x, normal.y, normal.z}, fraction * reach, surfaceIndexOf(c, side, point), entityOf(side)};
    return 1;
}

int32_t apiRaycast(void* c, const float* from, const float* direction, float maxDistance, FxSurfaceHit* out)
{
    const PhysicsWorld* physics = ctxOf(c).physics;
    if (!physics || !from || !direction || !out || maxDistance <= 0.0f)
        return 0;
    float3 d = {direction[0], direction[1], direction[2]};
    if (length(d) < 1e-9f)
        return 0;
    d = normalize(d) * maxDistance;
    ContactSide side;
    float3 point, normal;
    float fraction = 0.0f;
    if (!physics->castRay({from[0], from[1], from[2]}, d, side, point, normal, fraction))
        return 0;
    *out = {{point.x, point.y, point.z}, {normal.x, normal.y, normal.z}, fraction * maxDistance, surfaceIndexOf(c, side, point), entityOf(side)};
    return 1;
}

// ---- (v15) Prefabs, particles, decals

uint64_t apiPrefabSpawn(void* c, const char* path, const float* position, const float* rotation, uint64_t parent)
{
    Context& x = ctxOf(c);
    if (!x.prefabs || !x.scene || !path)
        return 0;
    // The rotation (x, y, z, w) and position as rows of a 3x4.
    float qx = 0, qy = 0, qz = 0, qw = 1;
    if (rotation) {
        const float n = std::sqrt(rotation[0] * rotation[0] + rotation[1] * rotation[1] + rotation[2] * rotation[2] + rotation[3] * rotation[3]);
        if (n > 1e-8f)
            qx = rotation[0] / n, qy = rotation[1] / n, qz = rotation[2] / n, qw = rotation[3] / n;
    }
    const float world[3][4] = {
        {1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw), position ? position[0] : 0.0f},
        {2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw), position ? position[1] : 0.0f},
        {2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy), position ? position[2] : 0.0f}};
    flecs::world& world_ = x.scene->world();
    const flecs::entity under = parent && ecs_is_alive(world_, parent) ? flecs::entity(world_, parent) : flecs::entity();
    std::vector<uint64_t> made;
    const flecs::entity root = x.prefabs->spawn(*x.scene, path, world, under, &made);
    if (x.tracking)
        for (const uint64_t e : made)
            trackCreated(x, e);
    return root ? root.id() : 0;
}

int32_t apiParticlesControl(void* c, uint64_t entity, int32_t op)
{
    Context& x = ctxOf(c);
    if (!x.particles || !x.scene || !ecs_is_alive(worldOf(c), entity))
        return 0;
    const flecs::entity e(x.scene->world(), entity);
    switch (op) {
    case 0: return x.particles->play(e) ? 1 : 0;
    case 1: return x.particles->stop(e, false) ? 1 : 0;
    case 2: return x.particles->stop(e, true) ? 1 : 0;
    case 3: return x.particles->restart(e) ? 1 : 0;
    default: return 0;
    }
}

int32_t apiParticlesEmit(void* c, uint64_t entity, int32_t count)
{
    Context& x = ctxOf(c);
    if (!x.particles || !x.scene || count <= 0 || !ecs_is_alive(worldOf(c), entity))
        return 0;
    return int32_t(x.particles->emit(flecs::entity(x.scene->world(), entity), uint32_t(count)));
}

int32_t apiParticlesCount(void* c, uint64_t entity)
{
    Context& x = ctxOf(c);
    if (!x.particles || !x.scene || !ecs_is_alive(worldOf(c), entity))
        return -1;
    const flecs::entity e(x.scene->world(), entity);
    return x.particles->effectOf(e) ? int32_t(x.particles->count(e)) : -1;
}

int32_t apiParticlesHits(void* c, uint64_t entity, FxParticleHit* out, int32_t capacity)
{
    Context& x = ctxOf(c);
    if (!x.particles || !x.scene || !ecs_is_alive(worldOf(c), entity))
        return 0;
    const ParticleSystem::Effect* fx = x.particles->effectOf(flecs::entity(x.scene->world(), entity));
    if (!fx)
        return 0;
    static_assert(sizeof(FxParticleHit) == sizeof(ParticleHit));
    const int32_t n = int32_t(fx->hits.size());
    if (out && capacity > 0)
        std::memcpy(out, fx->hits.data(), size_t(std::min(n, capacity)) * sizeof(FxParticleHit));
    return n;
}

void apiParticlesScript(void* c, uint64_t entity, const char* className)
{
    Context& x = ctxOf(c);
    if (!x.scene || !className || !ecs_is_alive(worldOf(c), entity))
        return;
    flecs::entity(x.scene->world(), entity).set<ParticleScript>({vfxAssetId(className)});
}

int32_t apiDecalPlace(void* c, const char* material, const float* point, const float* normal, float size, float depth, float angle,
                      float lifetime, uint64_t attach)
{
    Context& x = ctxOf(c);
    if (!x.placeDecal || !material || !point || !normal)
        return 0;
    return x.placeDecal(material, point, normal, size, depth, angle, lifetime, attach) ? 1 : 0;
}

void apiDecalsClear(void* c)
{
    if (ctxOf(c).clearDecals)
        ctxOf(c).clearDecals();
}

// The skinned record of the character an entity is: its own (MeshInstance), else the first below it
// (a model placed whole: its root drives its meshes through its Animator). A character handle
// (kFxCharacterTag) is its record already.
uint32_t recordOf(void* c, uint64_t entity)
{
    Context& x = ctxOf(c);
    if (!x.animation || !x.skinning)
        return ~0u;
    if (entity & kFxCharacterTag)
        return uint32_t(entity) < x.animation->characterCount() ? uint32_t(entity) : ~0u;
    if (!ecs_is_alive(worldOf(c), entity))
        return ~0u;
    if (const auto* mi = static_cast<const MeshInstance*>(ecs_get_id(worldOf(c), entity, x.idMeshInstance)))
        return x.skinning->recordOfInstance(mi->gpuSlot);
    if (const auto it = x.characterRecords.find(entity); it != x.characterRecords.end())
        return it->second;
    const std::vector<uint32_t> below = x.animation->recordsUnder(x.scene->world(), entity);
    return x.characterRecords[entity] = below.empty() ? ~0u : below.front();
}

anim::GraphInstance* graphOf(void* c, uint64_t entity)
{
    const uint32_t record = recordOf(c, entity);
    return record == ~0u ? nullptr : ctxOf(c).animation->graph(record);
}

uint64_t apiAnimCharacter(void* c, uint64_t entity)
{
    const uint32_t record = recordOf(c, entity);
    return record == ~0u || record >= ctxOf(c).animation->characterCount() ? 0 : kFxCharacterTag | record;
}

int32_t apiAnimParameter(void* c, uint64_t entity, const char* name)
{
    anim::GraphInstance* g = graphOf(c, entity);
    return g ? g->parameterIndex(name) : -1;
}

void apiAnimSetFloat(void* c, uint64_t entity, int32_t parameter, float value)
{
    if (anim::GraphInstance* g = graphOf(c, entity))
        g->setFloat(parameter, value);
}

void apiAnimSetBool(void* c, uint64_t entity, int32_t parameter, int32_t value)
{
    if (anim::GraphInstance* g = graphOf(c, entity))
        g->setBool(parameter, value != 0);
}

void apiAnimSetId(void* c, uint64_t entity, int32_t parameter, const char* id)
{
    if (anim::GraphInstance* g = graphOf(c, entity))
        g->setId(parameter, anim::fmt::hashId(id));
}

int32_t apiAnimEvents(void* c, uint64_t entity, FxAnimEvent* out, int32_t capacity)
{
    const uint32_t record = recordOf(c, entity);
    const std::vector<anim::SampledEvent>* events = record == ~0u ? nullptr : ctxOf(c).animation->events(record);
    if (!events)
        return 0;
    for (int32_t i = 0; out && i < capacity && size_t(i) < events->size(); ++i) {
        const anim::SampledEvent& e = (*events)[size_t(i)];
        out[i] = {e.id, e.weight, e.percent, e.kind, e.flags, uint8_t(e.active ? 1 : 0), 0};
    }
    return int32_t(events->size());
}

int32_t apiAnimHideBone(void* c, uint64_t entity, const char* bone, int32_t hidden)
{
    const uint32_t record = recordOf(c, entity);
    AnimationSystem* animation = ctxOf(c).animation;
    const anim::Pose* pose = record == ~0u ? nullptr : animation->pose(record);
    if (!bone || !pose || pose->skeleton()->findBone(bone) < 0)
        return -1;
    if (hidden >= 0)
        animation->hideBone(record, bone, hidden != 0);
    return animation->boneHidden(record, bone) ? 1 : 0;
}

int32_t apiAnimFootIk(void* c, uint64_t entity, float weight)
{
    const uint32_t record = recordOf(c, entity);
    return record != ~0u && ctxOf(c).animation->setFootIkWeight(record, weight) ? 1 : 0;
}

uint64_t apiAnimEntity(void* c, uint64_t entity, const char* name, int32_t op, uint64_t value)
{
    const uint32_t record = recordOf(c, entity);
    if (record == ~0u || !name)
        return ~0ull;
    AnimationSystem& a = *ctxOf(c).animation;
    if ((op == 1 && !a.setEntity(record, name, value)) || (op == 2 && !a.resetEntity(record, name)))
        return ~0ull;
    if (op == 0) {
        const anim::GraphInstance* g = a.graph(record);
        const auto& names = g ? g->definition().entityParameters : std::vector<std::string>{};
        if (!a.entity(record, name) && std::find(names.begin(), names.end(), name) == names.end())
            return ~0ull;
    }
    return a.entity(record, name);
}

int32_t apiAnimLookAt(void* c, uint64_t entity, const float* target, float weight, const char* tip)
{
    const uint32_t record = recordOf(c, entity);
    return record != ~0u && target &&
                   ctxOf(c).animation->lookAt(record, {target[0], target[1], target[2]}, weight, tip ? tip : "head")
               ? 1
               : 0;
}

int32_t apiAnimReach(void* c, uint64_t entity, const char* bone, const float* target, float weight)
{
    const uint32_t record = recordOf(c, entity);
    return record != ~0u && bone && target && ctxOf(c).animation->reach(record, bone, {target[0], target[1], target[2]}, weight) ? 1 : 0;
}

int32_t apiAnimAimAt(void* c, uint64_t entity, const float* target, float weight, const char* bone, const float* axis, const float* hold,
                     const char* support)
{
    const uint32_t record = recordOf(c, entity);
    const float3 held = hold ? float3{hold[0], hold[1], hold[2]} : float3{};
    return record != ~0u && target &&
                   ctxOf(c).animation->aimAt(record, {target[0], target[1], target[2]}, weight, bone ? bone : "hand_r",
                                             axis ? float3{axis[0], axis[1], axis[2]} : float3{}, hold ? &held : nullptr,
                                             support ? support : "")
               ? 1
               : 0;
}

// ---- v25: animation read back, terrain, water, particle fields, the UI's state

int32_t copyText(const std::string& s, char* buffer, int32_t capacity)
{
    const int32_t n = int32_t(s.size());
    if (!buffer || capacity < n)
        return -n;
    std::memcpy(buffer, s.data(), size_t(n));
    return n;
}

float apiAnimGetFloat(void* c, uint64_t entity, int32_t parameter)
{
    anim::GraphInstance* g = graphOf(c, entity);
    if (!g || parameter < 0 || uint32_t(parameter) >= g->parameterCount())
        return 0.0f;
    switch (g->parameterType(uint32_t(parameter))) {
    case anim::fmt::NodeType::ParamBool: return g->getBool(parameter) ? 1.0f : 0.0f;
    case anim::fmt::NodeType::ParamFloat: return g->getFloat(parameter);
    default: return 0.0f;
    }
}

uint32_t apiAnimGetId(void* c, uint64_t entity, int32_t parameter)
{
    anim::GraphInstance* g = graphOf(c, entity);
    return g && parameter >= 0 && uint32_t(parameter) < g->parameterCount() &&
                   g->parameterType(uint32_t(parameter)) == anim::fmt::NodeType::ParamId
               ? g->getId(parameter)
               : 0u;
}

int32_t apiAnimInState(void* c, uint64_t entity, const char* state)
{
    const anim::GraphInstance* g = graphOf(c, entity);
    return g && state && g->inState(anim::fmt::hashId(state)) ? 1 : 0;
}

int32_t apiAnimStateName(void* c, uint64_t entity, int32_t n, char* buffer, int32_t capacity)
{
    const anim::GraphInstance* g = graphOf(c, entity);
    return copyText(g && n >= 0 ? g->stateName(uint32_t(n)) : std::string(), buffer, capacity);
}

float apiAnimRate(void* c, uint64_t entity, float rate, int32_t set)
{
    const uint32_t record = recordOf(c, entity);
    if (record == ~0u)
        return 0.0f;
    if (set)
        ctxOf(c).animation->setRate(record, rate);
    return ctxOf(c).animation->rate(record);
}

int32_t apiAnimPlay(void* c, uint64_t entity, const char* clip, float speed, float fade, int32_t loop)
{
    const uint32_t record = recordOf(c, entity);
    return record != ~0u && ctxOf(c).animation->play(record, clip ? clip : "", speed, fade, loop != 0) ? 1 : 0;
}

float apiAnimPlaying(void* c, uint64_t entity)
{
    const uint32_t record = recordOf(c, entity);
    return record == ~0u ? -1.0f : ctxOf(c).animation->playing(record);
}

int32_t apiAnimRootMotion(void* c, uint64_t entity, float* out)
{
    const uint32_t record = recordOf(c, entity);
    if (record == ~0u || !out)
        return 0;
    const anim::Xform m = ctxOf(c).animation->rootMotion(record);
    const float v[7] = {m.t.x, m.t.y, m.t.z, m.r.x, m.r.y, m.r.z, m.r.w};
    std::memcpy(out, v, sizeof(v));
    return 1;
}

int32_t apiTerrainSample(void* c, const float* p, FxTerrainSample* out)
{
    const Terrain* t = ctxOf(c).terrain;
    if (!t || !t->valid() || !p || !out)
        return 0;
    const TerrainSettings& s = t->settings();
    const float last = float(t->samplesPerSide() - 1);
    const float fx = (p[0] - s.originX) / s.spacing, fz = (p[2] - s.originZ) / s.spacing;
    if (fx < 0.0f || fz < 0.0f || fx > last || fz > last)
        return 0;
    const float h = t->heightAt(p[0], p[2]), e = s.spacing;
    const float3 n = normalize(float3{t->heightAt(p[0] - e, p[2]) - t->heightAt(p[0] + e, p[2]), 2.0f * e,
                                      t->heightAt(p[0], p[2] - e) - t->heightAt(p[0], p[2] + e)});
    const uint32_t m = t->material(int32_t(std::lround(fx)), int32_t(std::lround(fz)));
    *out = {h, {n.x, n.y, n.z}, int32_t(terrain::materialA(m)), int32_t(terrain::materialB(m)),
            float(terrain::materialWeightB(m)) / 255.0f, 0};
    return 1;
}

int32_t apiTerrainRaycast(void* c, const float* o, const float* d, float maxDistance, float* t)
{
    const Terrain* terrain = ctxOf(c).terrain;
    if (!terrain || !terrain->valid() || !o || !d)
        return 0;
    const float3 dir = normalize(float3{d[0], d[1], d[2]});
    float hit = 0.0f;
    if (!terrain->raycast({o[0], o[1], o[2]}, dir, maxDistance, hit))
        return 0;
    if (t)
        *t = hit;
    return 1;
}

int32_t apiTerrainLayerName(void* c, int32_t layer, char* buffer, int32_t capacity)
{
    const Terrain* t = ctxOf(c).terrain;
    if (!t || layer < 0)
        return 0;
    const std::vector<TerrainLayer> layers = parseTerrainLayers(t->layersJson());
    return copyText(size_t(layer) < layers.size() ? layers[size_t(layer)].name : std::string(), buffer, capacity);
}

int32_t apiWaterBody(void* c, uint64_t entity)
{
    const WaterSystem* water = ctxOf(c).water;
    return water ? water->find(entity) : -1;
}

int32_t apiWaterNearestSurface(void* c, int32_t body, const float* p, float* out)
{
    const WaterSystem* water = ctxOf(c).water;
    const WaterSystem::Body* b = water && body >= 0 ? water->body(uint32_t(body)) : nullptr;
    if (!b || !p || !out)
        return 0;
    const float3 s = water->nearestSurface(*b, {p[0], p[1], p[2]});
    out[0] = s.x, out[1] = s.y, out[2] = s.z;
    return 1;
}

int32_t apiVfxComponent(void* c, uint64_t entity, const char* component, int32_t op)
{
    ecs_world_t* w = worldOf(c);
    const int32_t index = component ? vfxComponentIndex(component) : -1;
    if (index < 0 || !ecs_is_alive(w, entity))
        return 0;
    uint32_t count = 0;
    const VfxComponentInfo& info = vfxComponents(count)[index];
    const flecs::entity_t id = info.id(ctxOf(c).scene->world());
    if (op == 1 && !ecs_has_id(w, entity, id)) {
        std::vector<uint8_t> data(info.size);
        info.init(data.data());
        ecs_set_id(w, entity, id, info.size, data.data());
    } else if (op == 2) {
        ecs_remove_id(w, entity, id);
    }
    return ecs_has_id(w, entity, id) ? 1 : 0;
}

int32_t apiVfxField(void* c, uint64_t entity, const char* component, const char* field, const char* json, char* buffer,
                    int32_t capacity)
{
    ecs_world_t* w = worldOf(c);
    const int32_t index = component ? vfxComponentIndex(component) : -1;
    if (index < 0 || !field || !ecs_is_alive(w, entity))
        return -1;
    uint32_t count = 0;
    const VfxComponentInfo& info = vfxComponents(count)[index];
    const VfxField* f = nullptr;
    for (uint32_t i = 0; i < info.fieldCount && !f; ++i)
        if (std::strcmp(info.fields[i].key, field) == 0)
            f = &info.fields[i];
    const flecs::entity_t id = info.id(ctxOf(c).scene->world());
    if (!f || !ecs_has_id(w, entity, id))
        return -1;
    if (json) {
        std::vector<uint8_t> data(info.size);
        std::memcpy(data.data(), ecs_get_id(w, entity, id), info.size);
        Json value;
        try {
            value = Json::parse(json);
        } catch (...) {
            return -1;
        }
        if (!readVfxField(*f, data.data(), value))
            return -1;
        ecs_set_id(w, entity, id, info.size, data.data());  // (OnSet: the effect takes it)
    }
    JsonWriter out;
    out.beginObject();
    writeVfxField(out, *f, ecs_get_id(w, entity, id));
    out.endObject();
    // {"key":value} -> value
    std::string s = out.str();
    const size_t colon = s.find(':');
    s = colon == std::string::npos ? std::string() : s.substr(colon + 1, s.size() - colon - 2);
    return copyText(s, buffer, capacity);
}

uint32_t apiUiState(void* c)
{
    const UiLayer* ui = ctxOf(c).ui;
    return ui ? (ui->wantsPointer() ? 1u : 0u) | (ui->gameKeyboard() ? 0u : 2u) : 0u;
}

void apiMouseGet(void* c, FxMouse* out)
{
    if (out)
        *out = ctxOf(c).mouse;
}

void setMouseLock(Context& x, bool locked)
{
    if (x.mouse.locked == uint32_t(locked))
        return;
    x.mouse.locked = locked;
    SDL_Window* window = SDL_GetMouseFocus();
    if (!window)
        window = SDL_GetKeyboardFocus();
    if (window)
        SDL_SetWindowRelativeMouseMode(window, locked);
}

int32_t apiGamepadRumble(void* c, int32_t slot, float low, float high, uint32_t ms)
{
    Context& x = ctxOf(c);
    if (slot < 0 || slot >= 8 || !x.gamepadHandles[slot])
        return 0;
    const auto amount = [](float v) { return Uint16(std::clamp(v, 0.0f, 1.0f) * 65535.0f); };
    return SDL_RumbleGamepad(static_cast<SDL_Gamepad*>(x.gamepadHandles[slot]), amount(low), amount(high), ms) ? 1 : 0;
}

int32_t apiTextInput(void* c, char* buffer, int32_t capacity)
{
    const Context& x = ctxOf(c);
    const int32_t n = int32_t(x.text.size());
    if (!buffer || capacity < n)
        return -n;
    std::memcpy(buffer, x.text.data(), size_t(n));
    return n;
}

void apiTextMode(void* c, int32_t on)
{
    Context& x = ctxOf(c);
    if (x.textOn == (on != 0))
        return;
    x.textOn = on != 0;
    SDL_Window* window = SDL_GetKeyboardFocus();
    if (!window)
        window = SDL_GetMouseFocus();
    if (window && x.textOn)
        SDL_StartTextInput(window);
    else if (window)
        SDL_StopTextInput(window);
}

float apiTimeScale(void* c, float scale)
{
    Context& x = ctxOf(c);
    if (scale >= 0.0f)
        x.timeScale = std::min(scale, 100.0f);
    return x.timeScale;
}

int32_t copyText(const std::string& s, char* buffer, int32_t capacity);

void apiQuit(void* c) { ctxOf(c).quitRequested = true; }

int32_t apiSceneCurrent(void* c, char* buffer, int32_t capacity) { return copyText(ctxOf(c).scenePath, buffer, capacity); }

int32_t apiSceneLoad(void* c, const char* scene)
{
    Context& x = ctxOf(c);
    std::error_code ec;
    if (!scene || !*scene || x.projectDir.empty() || !std::filesystem::exists(std::filesystem::path(x.projectDir) / scene, ec))
        return 0;
    x.sceneNext = scene;
    return 1;
}

int32_t apiSceneLoadAsync(void* c, const char* scene)
{
    Context& x = ctxOf(c);
    std::error_code ec;
    if (!scene || !*scene || x.projectDir.empty() || !std::filesystem::exists(std::filesystem::path(x.projectDir) / scene, ec))
        return 0;
    x.sceneAsync = scene;
    x.loadProgress = 0.0f;  // (loading from now: the engine takes it after this frame's update)
    x.loadStage = "starting";
    return 1;
}

float apiSceneLoading(void* c, char* stage, int32_t capacity)
{
    Context& x = ctxOf(c);
    if (stage && capacity > 0) {
        const size_t n = std::min(x.loadStage.size(), size_t(capacity - 1));
        std::memcpy(stage, x.loadStage.data(), n);
        stage[n] = 0;
    }
    return x.loadProgress;
}

float apiCameraFov(void* c)
{
    const float p11 = ctxOf(c).camera.p11;
    return p11 > 0.0f ? 2.0f * std::atan(1.0f / p11) * 180.0f / 3.14159265f : 60.0f;
}

float apiRenderSetting(void* c, const char* key, float value, int32_t set)
{
    Context& x = ctxOf(c);
    if (!x.render || !key)
        return std::numeric_limits<float>::quiet_NaN();
    if (set && x.tracking && x.renderSaved.empty()) {  // (as they were: Stop puts them back)
        x.renderSaved.resize(sizeof(FrameSettings));
        std::memcpy(x.renderSaved.data(), x.render, sizeof(FrameSettings));
    }
    FrameSettings& s = *x.render;
    PostSettings& p = s.post;
    const std::string_view k = key;
    const bool b = value != 0.0f;
    const auto pick = [&](auto& field, auto v) {
        if (set)
            field = v;
        return float(field);
    };
    if (k == "exposure")
        return pick(p.exposureCompensation, value);
    if (k == "manualExposure")
        return pick(p.manualExposure, std::max(value, 0.0f));
    if (k == "bloom")
        return pick(p.bloomIntensity, std::max(value, 0.0f));
    if (k == "ao") {
        if (set)
            p.ao = AoMode(std::min(uint32_t(std::max(value, 0.0f)), 2u));
        return float(uint32_t(p.ao));
    }
    if (k == "aoRadius")
        return pick(p.aoRadius, std::max(value, 0.01f));
    if (k == "aoPower")
        return pick(p.aoPower, std::max(value, 0.0f));
    if (k == "lookPower")
        return pick(p.lookPower, std::max(value, 0.05f));
    if (k == "lookSaturation")
        return pick(p.lookSaturation, std::max(value, 0.0f));
    if (k == "gi")
        return pick(s.gi.enabled, b);
    if (k == "giIntensity")
        return pick(s.gi.intensity, std::max(value, 0.0f));
    if (k == "skyIntensity")
        return pick(s.gi.skyIntensity, std::max(value, 0.0f));
    if (k == "reflections")
        return pick(s.reflections, b);
    if (k == "reflectIntensity")
        return pick(s.reflect.intensity, std::clamp(value, 0.0f, 4.0f));
    if (k == "fxaa")
        return pick(p.fxaa, b);
    if (k == "taa")
        return pick(p.taa, b);
    if (k == "renderScale")
        return pick(p.renderScale, std::clamp(value, 0.25f, 1.0f));
    return std::numeric_limits<float>::quiet_NaN();
}

void apiDebugLine(void* c, const float* a, const float* b, uint32_t color, float seconds)
{
    Context& x = ctxOf(c);
    if (a && b && x.debugLines.size() < 65536)
        x.debugLines.push_back({{a[0], a[1], a[2]}, {b[0], b[1], b[2]}, color, seconds});
}

void apiGizmoLine(void* c, const float* a, const float* b, uint32_t color)
{
    Context& x = ctxOf(c);
    if (a && b && x.gizmoLines.size() < 65536)
        x.gizmoLines.push_back({{a[0], a[1], a[2]}, {b[0], b[1], b[2]}, color, 0.0f});
}

void apiMouseLock(void* c, int32_t locked)
{
    Context& x = ctxOf(c);
    if (!(locked && x.mouseFreed))  // (freed by Esc: held again after a click)
        setMouseLock(x, locked != 0);
}

// A name the game gave wins; an entity it renamed isn't found by its old name.
uint64_t apiEntityFind(void* c, const char* name)
{
    const Context& x = ctxOf(c);
    if (!name)
        return 0;
    for (const auto& [e, given] : x.names)
        if (given == name && ecs_is_alive(worldOf(c), e) && !ecs_has_id(worldOf(c), e, EcsDisabled))
            return e;
    const uint64_t found = x.findName ? x.findName(name) : 0;
    return x.names.count(found) ? 0 : found;
}

uint64_t apiEntityFindUnder(void* c, uint64_t root, const char* name)
{
    Context& x = ctxOf(c);
    if (!name || !root)
        return 0;
    if (!x.names.empty())
        for (const flecs::entity_t e : subtreeOf(x, root))
            if (e != root)
                if (const auto it = x.names.find(e); it != x.names.end() && it->second == name)
                    return e;
    const uint64_t found = x.findNameUnder ? x.findNameUnder(root, name) : 0;
    return x.names.count(found) ? 0 : found;
}

void apiUiSend(void* c, const char* utf8, int32_t length)
{
    if (!utf8 || length <= 0)
        return;
    if (UiLayer* ui = ctxOf(c).ui)
        ui->send(std::string_view(utf8, size_t(length)));
    else if (ctxOf(c).uiSend)  // (the editor: its page frames the game's)
        ctxOf(c).uiSend(std::string_view(utf8, size_t(length)));
}

int32_t apiUiPoll(void* c, char* buffer, int32_t capacity)
{
    ScriptHost::Context& x = ctxOf(c);
    if (x.uiMessage.empty() && !x.uiInbox.empty()) {  // (the editor's game UI)
        x.uiMessage = std::move(x.uiInbox.front());
        x.uiInbox.erase(x.uiInbox.begin());
    }
    if (x.uiMessage.empty() && !(x.ui && x.ui->poll(x.uiMessage)))
        return 0;
    const int32_t n = int32_t(x.uiMessage.size());
    if (!buffer || capacity < n)
        return -n;
    std::memcpy(buffer, x.uiMessage.data(), size_t(n));
    x.uiMessage.clear();
    return n;
}

void apiPerfStats(void* c, FxPerfStats* out)
{
    const Context& x = ctxOf(c);
    const SystemUsage u = systemstats::latest();
    *out = {x.frameMs,      x.frameMsAvg,  x.frameMsMax,     x.fps,         u.cpuProcess,
            u.cpuSystem,    u.gpuProcess,  u.gpuSystem,      u.ramProcessMB, u.ramUsedMB,
            u.ramTotalMB,   u.vramProcessMB, u.vramTotalMB, u.ramCommittedMB};
}

void apiHudSet(void* c, const char* utf8, int32_t length)
{
    Context& x = ctxOf(c);
    std::string text = utf8 && length > 0 ? std::string(utf8, size_t(length)) : std::string();
    if (text != x.hud) {
        x.hud = std::move(text);
        ++x.hudRevision;
    }
}

// A PE image with a CLI header is a managed assembly (JIT); anything else is native (AOT).
// ---- (v20) Script components

int32_t apiScriptPoll(void* c, char* buffer, int32_t capacity)
{
    Context& x = ctxOf(c);
    if (x.pendingScripts.empty())
        return 0;
    JsonWriter w;
    w.beginArray();
    for (const auto& [entity, e] : x.pendingScripts)
        w.beginObject().value("entity", entity).value("class", e.className).value("enabled", e.enabled).raw("properties", e.properties).endObject();
    w.endArray();
    const int32_t n = int32_t(w.str().size());
    if (!buffer || capacity < n)
        return -n;
    std::memcpy(buffer, w.str().data(), size_t(n));
    x.pendingScripts.clear();
    return n;
}

uint64_t apiScriptEntityByKey(void* c, const char* key)
{
    const Context& x = ctxOf(c);
    return key && *key && x.entityByKey ? x.entityByKey(key) : 0;
}

// ---- (v19) Physics

int32_t apiBodyGet(void* c, uint64_t entity, FxBodyState* out)
{
    b3BodyId id;
    if (!out || !bodyOf(c, entity, id))
        return 0;
    const b3Vec3 v = b3Body_GetLinearVelocity(id), w = b3Body_GetAngularVelocity(id);
    const b3MotionLocks l = b3Body_GetMotionLocks(id);
    const b3BodyType t = b3Body_GetType(id);
    *out = {{v.x, v.y, v.z}, {w.x, w.y, w.z}, b3Body_GetMass(id), b3Body_GetGravityScale(id), b3Body_GetLinearDamping(id),
            b3Body_GetAngularDamping(id), t == b3_dynamicBody ? 2u : t == b3_kinematicBody ? 1u : 0u,
            uint32_t(l.linearX) | uint32_t(l.linearY) << 1 | uint32_t(l.linearZ) << 2 | uint32_t(l.angularX) << 3 |
                uint32_t(l.angularY) << 4 | uint32_t(l.angularZ) << 5,
            b3Body_IsAwake(id) ? 1u : 0u, b3Body_IsBullet(id) ? 1u : 0u};
    return 1;
}

int32_t apiBodySet(void* c, uint64_t entity, const FxBodyState* in, uint32_t mask)
{
    Context& x = ctxOf(c);
    b3BodyId id;
    if (!in || !bodyOf(c, entity, id))
        return 0;
    if ((mask & kFxBodyType) && x.physics)
        x.physics->setBodyType(entity, in->type);
    if (mask & kFxBodyLinear)
        b3Body_SetLinearVelocity(id, {in->linear[0], in->linear[1], in->linear[2]});
    if (mask & kFxBodyAngular)
        b3Body_SetAngularVelocity(id, {in->angular[0], in->angular[1], in->angular[2]});
    if (mask & kFxBodyMass) {  // (its shapes' distribution kept)
        b3MassData md = b3Body_GetMassData(id);
        if (md.mass > 0.0f && in->mass > 0.0f) {
            const float k = in->mass / md.mass;
            md.mass = in->mass;
            md.inertia.cx = b3MulSV(k, md.inertia.cx);
            md.inertia.cy = b3MulSV(k, md.inertia.cy);
            md.inertia.cz = b3MulSV(k, md.inertia.cz);
            b3Body_SetMassData(id, md);
        }
    }
    if (mask & kFxBodyGravityScale)
        b3Body_SetGravityScale(id, in->gravityScale);
    if (mask & kFxBodyDamping) {
        b3Body_SetLinearDamping(id, std::max(in->linearDamping, 0.0f));
        b3Body_SetAngularDamping(id, std::max(in->angularDamping, 0.0f));
    }
    if (mask & kFxBodyLocks) {
        b3MotionLocks l = {};
        l.linearX = in->locks & 1, l.linearY = (in->locks >> 1) & 1, l.linearZ = (in->locks >> 2) & 1;
        l.angularX = (in->locks >> 3) & 1, l.angularY = (in->locks >> 4) & 1, l.angularZ = (in->locks >> 5) & 1;
        b3Body_SetMotionLocks(id, l);
    }
    if (mask & kFxBodyBullet)
        b3Body_SetBullet(id, in->bullet != 0);
    if (mask & kFxBodyAwake)
        b3Body_SetAwake(id, in->awake != 0);
    else if (mask & (kFxBodyLinear | kFxBodyAngular))
        b3Body_SetAwake(id, true);
    return 1;
}

int32_t apiBodyPush(void* c, uint64_t entity, const float* force, const float* point, const float* torque, int32_t mode)
{
    b3BodyId id;
    if (!bodyOf(c, entity, id) || b3Body_GetType(id) != b3_dynamicBody)
        return 0;
    if (force) {
        const b3Vec3 f = {force[0], force[1], force[2]};
        if (point) {
            b3Pos p;
            p.x = point[0], p.y = point[1], p.z = point[2];
            mode == 1 ? b3Body_ApplyLinearImpulse(id, f, p, true) : b3Body_ApplyForce(id, f, p, true);
        } else {
            mode == 1 ? b3Body_ApplyLinearImpulseToCenter(id, f, true) : b3Body_ApplyForceToCenter(id, f, true);
        }
    }
    if (torque) {
        const b3Vec3 t = {torque[0], torque[1], torque[2]};
        mode == 1 ? b3Body_ApplyAngularImpulse(id, t, true) : b3Body_ApplyTorque(id, t, true);
    }
    return 1;
}

int32_t apiBodyTeleport(void* c, uint64_t entity, const float* position, const float* rotation)
{
    PhysicsWorld* physics = ctxOf(c).physics;
    return physics && position && physics->teleport(entity, {position[0], position[1], position[2]}, rotation) ? 1 : 0;
}

int32_t apiBodyRebuild(void* c, uint64_t entity)
{
    PhysicsWorld* physics = ctxOf(c).physics;
    return physics && physics->rebuild(entity) ? 1 : 0;
}

void apiPhysicsGravity(void* c, float* get, const float* set)
{
    Context& x = ctxOf(c);
    if (!x.physics)
        return;
    const float3 g = x.physics->gravity();
    if (get)
        get[0] = g.x, get[1] = g.y, get[2] = g.z;
    if (set) {
        if (x.tracking && !x.gravitySet) {
            x.gravitySet = true;
            x.gravity[0] = g.x, x.gravity[1] = g.y, x.gravity[2] = g.z;
        }
        x.physics->setGravity({set[0], set[1], set[2]});
    }
}

QueryFilter filterOf(const FxQueryFilter* f) { return f ? QueryFilter{f->layers, f->ignore} : QueryFilter{}; }

QueryShape shapeOf(const FxQueryShape& s)
{
    QueryShape q;
    q.type = QueryShape::Type(std::min(s.type, 2u));
    q.size = {s.size[0], s.size[1], s.size[2]};
    q.position = {s.position[0], s.position[1], s.position[2]};
    std::memcpy(q.rotation, s.rotation, sizeof(q.rotation));
    return q;
}

int32_t apiRaycastFiltered(void* c, const float* from, const float* direction, float maxDistance, const FxQueryFilter* filter,
                           FxSurfaceHit* out)
{
    const PhysicsWorld* physics = ctxOf(c).physics;
    if (!physics || !from || !direction || !out || !(maxDistance > 0.0f))
        return 0;
    float3 d = {direction[0], direction[1], direction[2]};
    if (length(d) < 1e-9f)
        return 0;
    d = normalize(d) * maxDistance;
    ContactSide side;
    float3 point, normal;
    float fraction = 0.0f;
    if (!physics->castRayFiltered({from[0], from[1], from[2]}, d, filterOf(filter), side, point, normal, fraction))
        return 0;
    *out = {{point.x, point.y, point.z}, {normal.x, normal.y, normal.z}, fraction * maxDistance, surfaceIndexOf(c, side, point), entityOf(side)};
    return 1;
}

int32_t apiShapeCast(void* c, const FxQueryShape* shape, const float* direction, float maxDistance, const FxQueryFilter* filter,
                     FxSurfaceHit* out)
{
    const PhysicsWorld* physics = ctxOf(c).physics;
    if (!physics || !shape || !direction || !out || !(maxDistance > 0.0f))
        return 0;
    float3 d = {direction[0], direction[1], direction[2]};
    if (length(d) < 1e-9f)
        return 0;
    d = normalize(d) * maxDistance;
    ContactSide side;
    float3 point, normal;
    float fraction = 0.0f;
    if (!physics->castShape(shapeOf(*shape), d, filterOf(filter), side, point, normal, fraction))
        return 0;
    *out = {{point.x, point.y, point.z}, {normal.x, normal.y, normal.z}, fraction * maxDistance, surfaceIndexOf(c, side, point), entityOf(side)};
    return 1;
}

int32_t apiOverlap(void* c, const FxQueryShape* shape, const FxQueryFilter* filter, uint64_t* out, int32_t capacity)
{
    const PhysicsWorld* physics = ctxOf(c).physics;
    return physics && shape ? physics->overlap(shapeOf(*shape), filterOf(filter), out, capacity) : 0;
}

int32_t apiTriggerPoll(void* c, FxTrigger* out, int32_t capacity)
{
    const PhysicsWorld* physics = ctxOf(c).physics;
    if (!physics)
        return 0;
    const std::vector<TriggerEvent>& events = physics->triggers();
    const size_t from = ctxOf(c).stepEvents ? std::min(physics->stepTriggersBegin(), events.size()) : 0;  // (phase 6: the step's)
    for (int32_t i = 0; out && i < capacity && from + size_t(i) < events.size(); ++i) {
        const TriggerEvent& t = events[from + size_t(i)];
        out[i] = {t.trigger, entityOf(t.other), t.begin ? 1u : 0u, 0u};
    }
    return int32_t(events.size() - from);
}

int32_t apiCharacterMove(void* c, uint64_t entity, const float* position, float radius, float height, float stepHeight, float slopeLimit,
                         const float* delta, const FxQueryFilter* filter, FxCharacterMove* out)
{
    const PhysicsWorld* physics = ctxOf(c).physics;
    if (!physics || !position || !delta || !out)
        return 0;
    QueryFilter f = filterOf(filter);
    if (entity)
        f.ignore = entity;
    const float slopeCos = dmath::cos(std::clamp(slopeLimit, 0.0f, 89.0f) * 3.14159265f / 180.0f);
    const CharacterMove m = physics->moveCharacter({position[0], position[1], position[2]}, radius, height, std::max(stepHeight, 0.0f),
                                                   slopeCos, {delta[0], delta[1], delta[2]}, f);
    *out = {{m.position.x, m.position.y, m.position.z}, m.grounded ? 1u : 0u, {m.groundNormal.x, m.groundNormal.y, m.groundNormal.z},
            m.blocked ? 1u : 0u, entityOf(m.ground)};
    return 1;
}

uint64_t apiJointCreate(void* c, int32_t type, uint64_t a, uint64_t b, const FxJointSettings* s)
{
    PhysicsWorld* physics = ctxOf(c).physics;
    if (!physics || !s || type < 0 || type > 5)
        return 0;
    PhysicsWorld::JointSettings j;
    j.anchor = {s->anchor[0], s->anchor[1], s->anchor[2]};
    j.axis = {s->axis[0], s->axis[1], s->axis[2]};
    j.anchorB = {s->anchorB[0], s->anchorB[1], s->anchorB[2]};
    j.limit = s->limit != 0, j.lower = s->lower, j.upper = s->upper;
    j.motor = s->motor != 0, j.motorSpeed = s->motorSpeed, j.motorMax = s->motorMax;
    j.spring = s->spring, j.damping = s->damping, j.collide = s->collide != 0;
    return physics->createJoint(PhysicsWorld::JointType(type), a, b, j);
}

int32_t apiJointDestroy(void* c, uint64_t joint)
{
    PhysicsWorld* physics = ctxOf(c).physics;
    return physics && physics->destroyJoint(joint) ? 1 : 0;
}

void apiJointMotor(void* c, uint64_t joint, float speed, float maxForce)
{
    if (PhysicsWorld* physics = ctxOf(c).physics)
        physics->setJointMotor(joint, speed, maxForce);
}

// ---- (v18) Entities: enabled, names, hierarchy, transforms; rendering; meshes; refs

int32_t apiEntityEnabled(void* c, uint64_t entity, int32_t op)
{
    Context& x = ctxOf(c);
    ecs_world_t* w = worldOf(c);
    if (!ecs_is_alive(w, entity))
        return -1;
    if (op == 2 && !ecs_has_id(w, entity, EcsDisabled)) {
        // Disabled with everything below it that is enabled now: what enabling it again brings back.
        for (const flecs::entity_t id : subtreeOf(x, entity))
            if (ecs_is_alive(w, id) && !ecs_has_id(w, id, EcsDisabled)) {
                setEntityEnabled(x, id, false);
                x.scriptDisabled.insert(id);
            }
    } else if (op == 1 && x.scriptDisabled.count(entity)) {
        for (const flecs::entity_t id : subtreeOf(x, entity))
            if (x.scriptDisabled.erase(id) && ecs_is_alive(w, id))
                setEntityEnabled(x, id, true);
    }
    return ecs_has_id(w, entity, EcsDisabled) ? 0 : 1;
}

int32_t apiEntityName(void* c, uint64_t entity, char* buffer, int32_t capacity)
{
    Context& x = ctxOf(c);
    std::string name;
    if (const auto it = x.names.find(entity); it != x.names.end())
        name = it->second;
    else if (x.nameOf && ecs_is_alive(worldOf(c), entity))
        name = x.nameOf(entity);
    const int32_t n = int32_t(name.size());
    if (!buffer || capacity < n)
        return -n;
    std::memcpy(buffer, name.data(), size_t(n));
    return n;
}

void apiEntitySetName(void* c, uint64_t entity, const char* name)
{
    if (ecs_is_alive(worldOf(c), entity))
        ctxOf(c).names[entity] = name ? name : "";
}

uint64_t apiEntityParent(void* c, uint64_t entity)
{
    ecs_world_t* w = worldOf(c);
    if (!ecs_is_alive(w, entity))
        return 0;
    const auto* p = static_cast<const Parent*>(ecs_get_id(w, entity, ctxOf(c).idParent));
    return p && p->entity && ecs_is_alive(w, p->entity) ? p->entity : 0;
}

// Under `parent` (0: a root again), keeping where it is in the world (or its world transform taken
// as its local one). 0 when it can't be: dead, or the parent below it.
int32_t apiEntitySetParent(void* c, uint64_t entity, uint64_t parent, int32_t keepWorld)
{
    Context& x = ctxOf(c);
    ecs_world_t* w = worldOf(c);
    if (!ecs_is_alive(w, entity) || (parent && (!ecs_is_alive(w, parent) || parent == entity)))
        return 0;
    if (parent)
        for (const flecs::entity_t id : subtreeOf(x, entity))
            if (id == parent)
                return 0;
    flecs::world& world = x.scene->world();
    const flecs::entity child(world, entity);
    if (!parent || keepWorld) {
        x.scene->setParent(child, parent ? flecs::entity(world, parent) : flecs::entity());
        return 1;
    }
    float local[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    if (const WorldTransform* t = child.try_get<WorldTransform>())
        std::memcpy(local, t->rows, sizeof(local));
    x.scene->setLocal(child, flecs::entity(world, parent), local);
    return 1;
}

int32_t apiEntityChildren(void* c, uint64_t entity, uint64_t* out, int32_t capacity)
{
    Context& x = ctxOf(c);
    std::vector<flecs::entity_t> children;
    if (ecs_is_alive(worldOf(c), entity))
        x.scene->childrenOf(entity, children);
    for (int32_t i = 0; out && i < capacity && size_t(i) < children.size(); ++i)
        out[i] = children[size_t(i)];
    return int32_t(children.size());
}

// Moves an entity in the world (its local transform follows when it has a parent).
void apiEntitySetWorld(void* c, uint64_t entity, const float* rows)
{
    Context& x = ctxOf(c);
    if (rows && ecs_is_alive(worldOf(c), entity))
        x.scene->setWorld(flecs::entity(x.scene->world(), entity), reinterpret_cast<const float(*)[4]>(rows));
}

// What: 0 visible, 1 casts shadows, 2 tint (RGBA8, 0: none). Returns 0 when the entity draws no
// row of its own (not a mesh, or a spawned body).
int32_t apiRenderGet(void* c, uint64_t entity, int32_t what, uint32_t* value)
{
    Context& x = ctxOf(c);
    const uint32_t slot = rowOf(x, entity);
    if (slot == ~0u || !value)
        return 0;
    const fpak::InstanceGpu& row = x.pak->instances()[slot];
    const auto it = x.rows.find(slot);
    switch (what) {
    case 0:
        *value = spawnedRow(x, slot) ? !x.runtimeMeshes->hidden(entity) : !(it != x.rows.end() && (it->second.hide & 2));
        break;
    case 1: *value = (row.flags & fpak::kInstanceNoShadow) ? 0u : 1u; break;
    case 2: *value = row.tint; break;
    default: return 0;
    }
    return 1;
}

int32_t apiRenderSet(void* c, uint64_t entity, int32_t what, uint32_t value)
{
    Context& x = ctxOf(c);
    const uint32_t slot = rowOf(x, entity);
    if (slot == ~0u || what < 0 || what > 2)
        return 0;
    if (what == 0) {
        if (spawnedRow(x, slot))
            x.runtimeMeshes->setHidden(entity, value == 0);
        else
            hideRow(x, slot, 2, value == 0);
        return 1;
    }
    if (!spawnedRow(x, slot))
        rowState(x, slot);  // (kept as it was: Stop puts it back)
    fpak::InstanceGpu& row = x.pak->mutableInstances()[slot];
    if (what == 1)
        row.flags = (row.flags & ~uint32_t(fpak::kInstanceNoShadow)) | (value ? 0u : uint32_t(fpak::kInstanceNoShadow));
    else
        row.tint = value;
    x.touched.push_back(slot);
    return 1;
}

// The material an entity's mesh draws with: an .fmat (project path), or null for its own. 0 when
// it draws no row of its own, or the material can't be had (no room for it this session).
int32_t apiMaterialFind(void* c, const char* source)
{
    Context& x = ctxOf(c);
    std::error_code ec;
    if (!x.materials || !source || !*source ||  // (an asset that is there: cooked into the scene, or in the project)
        (!x.materials->cooked(source) && (x.projectDir.empty() || !std::filesystem::exists(std::filesystem::path(x.projectDir) / source, ec))))
        return -1;
    return x.materials->indexOf(source);
}

int32_t apiMaterialParam(void* c, int32_t material, const char* name, float* values, int32_t set)
{
    MaterialLibrary* lib = ctxOf(c).materials;
    const fpak::MaterialGpu* now = lib && material >= 0 && name && values ? lib->gpu(uint32_t(material)) : nullptr;
    if (!now)
        return 0;
    fpak::MaterialGpu m = *now;
    const std::string_view k = name;
    const auto field = [&](float* f, int32_t n) {
        for (int32_t i = 0; i < n; ++i)
            set ? (void)(f[i] = values[i]) : (void)(values[i] = f[i]);
        return n;
    };
    if (k == "params") {  // (R4: the surface shader's values)
        float p[4];
        std::copy(lib->params(uint32_t(material)), lib->params(uint32_t(material)) + 4, p);
        field(p, 4);
        if (set)
            lib->setParams(uint32_t(material), p);
        return 4;
    }
    int32_t n = 0;
    if (k == "baseColor")
        n = field(m.baseColor, 4);
    else if (k == "emissive")
        n = field(m.emissive, 3);
    else if (k == "emissiveStrength")
        n = field(&m.emissive[3], 1);
    else if (k == "roughness")
        n = field(&m.roughness, 1);
    else if (k == "roughnessMin") {  // (vtSize's top byte, in 255ths)
        float r = fpak::roughnessMinOf(m);
        n = field(&r, 1);
        m.vtSize = (m.vtSize & ~fpak::kVtRoughnessMinMask) | fpak::roughnessMinBits(r);
    } else if (k == "metallic")
        n = field(&m.metallic, 1);
    else if (k == "normalScale")
        n = field(&m.normalScale, 1);
    else if (k == "occlusionStrength")
        n = field(&m.occlusionStrength, 1);
    else if (k == "alphaCutoff")
        n = field(&m.alphaCutoff, 1);
    if (n && set) {
        m.roughness = std::clamp(m.roughness, 0.0f, 1.0f);
        m.metallic = std::clamp(m.metallic, 0.0f, 1.0f);
        lib->set(uint32_t(material), m);
    }
    return n;
}

int32_t apiMaterialInstance(void* c, uint64_t entity)
{
    Context& x = ctxOf(c);
    const uint32_t slot = rowOf(x, entity);
    if (slot == ~0u || !x.materials || !x.pak)
        return -1;
    fpak::InstanceGpu& row = x.pak->mutableInstances()[slot];
    int32_t current = int32_t(row.flags >> fpak::kInstanceMaterialShift) - 1;
    if (current < 0) {  // (its mesh's first)
        uint32_t meshCount = 0, clusterCount = 0;
        const auto* meshes = x.pak->view<fpak::MeshGpu>(fpak::Section::Meshes, &meshCount);
        const auto* clusters = x.pak->view<fpak::ClusterGpu>(fpak::Section::Clusters, &clusterCount);
        if (row.mesh >= meshCount || meshes[row.mesh].clusterOffset >= clusterCount)
            return -1;
        current = int32_t(clusters[meshes[row.mesh].clusterOffset].counts >> 16);
    }
    const int32_t copy = x.materials->clone(uint32_t(current));
    if (copy < 0)
        return -1;
    if (!spawnedRow(x, slot))
        rowState(x, slot);
    row.flags = (row.flags & 0xffffu) | (uint32_t(copy) + 1) << fpak::kInstanceMaterialShift;
    x.touched.push_back(slot);
    return copy;
}

uint32_t apiPostCreate(void* c, const char* shader, int32_t stage, int32_t order)
{
    Context& x = ctxOf(c);
    if (!shader || !*shader || x.projectDir.empty())
        return 0;
    Context::PostEffect e;
    e.blob = ProjectShaders::blobPath(x.projectDir, shader);
    ShaderFile file;
    std::string error;
    const ShaderFile::Entry* entry = loadShaderFile(e.blob, file, error) ? file.entry() : nullptr;
    if (!entry || entry->blob.stage != ShaderStage::Compute) {
        FX_WARN("script: PostEffect(%s): %s", shader, entry ? "not a compute shader" : error.c_str());
        return 0;
    }
    e.stage = std::clamp(stage, 0, 1);
    e.order = order;
    for (const ShaderFile::Uniform& u : entry->uniforms)
        e.fields.push_back({u.name, {u.offset, u.components, u.type == "int" ? 1u : u.type == "uint" ? 2u : 0u}});
    e.params.assign(std::max<size_t>(entry->uniformSize, 16), 0);
    const uint32_t id = x.nextPostEffect++;
    x.postEffects[id] = std::move(e);
    return id;
}

int32_t apiPostSet(void* c, uint32_t effect, const char* name, const float* values, int32_t count)
{
    Context& x = ctxOf(c);
    const auto it = x.postEffects.find(effect);
    if (it == x.postEffects.end() || !name || !values || count <= 0)
        return 0;
    for (const auto& [field, f] : it->second.fields) {
        if (field != name)
            continue;
        const uint32_t n = std::min(uint32_t(count), f[1]);
        for (uint32_t i = 0; i < n && f[0] + (i + 1) * 4 <= it->second.params.size(); ++i) {
            uint8_t* at = it->second.params.data() + f[0] + i * 4;
            if (f[2] == 1) {
                const int32_t v = int32_t(std::lround(values[i]));
                std::memcpy(at, &v, 4);
            } else if (f[2] == 2) {
                const uint32_t v = uint32_t(std::max(std::lround(values[i]), 0l));
                std::memcpy(at, &v, 4);
            } else {
                std::memcpy(at, &values[i], 4);
            }
        }
        return int32_t(n);
    }
    return 0;
}

void apiPostState(void* c, uint32_t effect, int32_t enabled, int32_t order)
{
    Context& x = ctxOf(c);
    if (const auto it = x.postEffects.find(effect); it != x.postEffects.end()) {
        it->second.enabled = enabled != 0;
        it->second.order = order;
    }
}

void apiPostDestroy(void* c, uint32_t effect) { ctxOf(c).postEffects.erase(effect); }

uint32_t apiMeshCreate(void* c, const void* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount)
{
    Context& x = ctxOf(c);
    const uint32_t id =
        x.meshes ? x.meshes->create(static_cast<const MeshRenderer::Vertex*>(vertices), vertexCount, indices, indexCount) : 0u;
    if (id)
        x.gameMeshes.push_back(id);
    return id;
}

int32_t apiMeshUpdate(void* c, uint32_t mesh, const void* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount)
{
    Context& x = ctxOf(c);
    return x.meshes && x.meshes->update(mesh, static_cast<const MeshRenderer::Vertex*>(vertices), vertexCount, indices, indexCount) ? 1 : 0;
}

void apiMeshDestroy(void* c, uint32_t mesh)
{
    Context& x = ctxOf(c);
    if (!x.meshes)
        return;
    x.meshes->destroy(mesh);
    std::erase(x.gameMeshes, mesh);
}

void apiMeshDraw(void* c, uint32_t mesh, const float* world, const float* color, const float* emissive, float roughness, float metallic)
{
    Context& x = ctxOf(c);
    if (!x.meshes || !world)
        return;
    MeshRenderer::Draw d = {};
    d.mesh = mesh;
    std::memcpy(d.world, world, sizeof(d.world));
    for (int i = 0; i < 4; ++i) {
        d.color[i] = color ? color[i] : 1.0f;
        d.emissive[i] = emissive ? emissive[i] : 0.0f;
    }
    d.surface[0] = roughness;
    d.surface[1] = metallic;
    x.meshes->queue(d);
}

int32_t apiRenderMaterial(void* c, uint64_t entity, const char* material)
{
    Context& x = ctxOf(c);
    const uint32_t slot = rowOf(x, entity);
    if (slot == ~0u || !x.runtimeMeshes)
        return 0;
    const bool named = material && *material;
    const int32_t m = named ? x.runtimeMeshes->materialOf(material) : -1;
    if (named && m < 0)
        return 0;
    if (!spawnedRow(x, slot))
        rowState(x, slot);
    fpak::InstanceGpu& row = x.pak->mutableInstances()[slot];
    row.flags = (row.flags & 0xffffu) | (m >= 0 ? (uint32_t(m) + 1) << fpak::kInstanceMaterialShift : 0u);
    x.touched.push_back(slot);
    return 1;
}

// A mesh of the scene (by name) drawn at a world transform, from the slots reserved when the scene
// was uploaded (vfx/runtime_meshes.h), with a material (.fmat; null: its own), under `parent`.
uint64_t apiMeshSpawn(void* c, const char* mesh, const float* rows, const char* material, uint64_t parent)
{
    Context& x = ctxOf(c);
    if (!mesh || !rows || !x.runtimeMeshes || !x.pak)
        return 0;
    const int32_t index = x.pak->findMesh(mesh);
    if (index < 0 || !x.runtimeMeshes->has(uint32_t(index))) {
        if (x.meshWarnings++ < 8)
            FX_WARN("script: Scene.SpawnMesh(%s): %s", mesh,
                    index < 0 ? "no such mesh in the scene" : "no slots for it this session (a prefab or a Model Renderer using it reserves them)");
        return 0;
    }
    const int32_t m = material && *material ? x.runtimeMeshes->materialOf(material) : -1;
    const flecs::entity e = x.runtimeMeshes->spawn(*x.scene, *x.pak, uint32_t(index), reinterpret_cast<const float(*)[4]>(rows), m, false);
    if (!e)
        return 0;
    trackCreated(x, e.id());
    if (parent && ecs_is_alive(worldOf(c), parent))
        x.scene->setParent(e, flecs::entity(x.scene->world(), parent));
    return e.id();
}

static_assert(sizeof(ecs_ref_t) <= sizeof(FxRef), "FxRef holds a flecs ref");

void apiRefInit(void* c, FxRef* ref, uint64_t entity, uint64_t component)
{
    if (!ref)
        return;
    *ref = {};
    if (ecs_is_alive(worldOf(c), entity))
        *reinterpret_cast<ecs_ref_t*>(ref) = ecs_ref_init_id(worldOf(c), entity, component);
}

const void* apiRefGet(void* c, FxRef* ref, uint64_t component)
{
    auto* r = reinterpret_cast<ecs_ref_t*>(ref);  // (a deleted entity changes its table: the ref finds it gone)
    return r && r->entity ? ecs_ref_get_id(worldOf(c), r, component) : nullptr;
}

// A .NET assembly (IL) rather than a native library: a PE file whose CLR header directory is set.
// Assemblies are PE files on every OS, so the fields are read by offset rather than through
// windows.h's structs.
bool isManagedImage(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    const auto read = [&f](std::streamoff at, void* out, size_t n) {
        f.seekg(at);
        return bool(f.read(static_cast<char*>(out), std::streamsize(n)));
    };
    uint16_t mz = 0, magic = 0;
    uint32_t peOffset = 0, signature = 0, clrHeader = 0;
    if (!read(0, &mz, 2) || mz != 0x5A4D || !read(0x3C, &peOffset, 4))  // "MZ", e_lfanew
        return false;
    if (!read(peOffset, &signature, 4) || signature != 0x00004550)  // the PE signature
        return false;
    const std::streamoff optional = std::streamoff(peOffset) + 4 + 20;  // after the COFF file header
    if (!read(optional, &magic, 2) || (magic != 0x10B && magic != 0x20B))  // PE32 (AnyCPU IL) / PE32+
        return false;
    // Data directory 14 (the CLR runtime header): its virtual address.
    const std::streamoff directories = optional + (magic == 0x10B ? 96 : 112);
    return read(directories + 14 * 8, &clrHeader, 4) && clrHeader != 0;
}

// hostfxr's strings: UTF-16 on Windows, UTF-8 elsewhere (char_t), as the OS names files.
std::basic_string<char_t> hostString(const std::filesystem::path& p) { return p.native(); }
// A literal in hostfxr's strings (wide on Windows, UTF-8 elsewhere).
#if defined(_WIN32)
#define FX_HOST_STR(s) L##s
#else
#define FX_HOST_STR(s) s
#endif

}  // namespace

bool ScriptHost::beginLoad(const char* gamePath, bool hotReload)
{
    path_ = std::filesystem::absolute(gamePath).string();
    hotReload_ = hotReload;
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) {
        FX_ERROR("script: %s does not exist", path_.c_str());
        return false;
    }
    jit_ = isManagedImage(path_);
    stamp_ = std::filesystem::last_write_time(path_, ec);

    native_.version = kFxScriptApiVersion;
    native_.size = sizeof(FxNativeApi);
    native_.context = &context_;  // filled in by start(); nothing calls back before OnStart
    native_.log = apiLog;
    native_.componentId = apiComponentId;
    native_.entityCreate = apiEntityCreate;
    native_.entityDestroy = apiEntityDestroy;
    native_.entityAlive = apiEntityAlive;
    native_.entitySet = apiEntitySet;
    native_.entityGet = apiEntityGet;
    native_.entityRemove = apiEntityRemove;
    native_.queryCreate = apiQueryCreate;
    native_.queryDestroy = apiQueryDestroy;
    native_.queryNext = apiQueryNext;
    native_.queryEnd = apiQueryEnd;
    native_.meshBounds = apiMeshBounds;
    native_.cameraGet = apiCameraGet;
    native_.bodySpawn = apiBodySpawn;
    native_.bodyImpulse = apiBodyImpulse;
    native_.bodyVelocity = apiBodyVelocity;
    native_.animParameter = apiAnimParameter;
    native_.animSetFloat = apiAnimSetFloat;
    native_.animSetBool = apiAnimSetBool;
    native_.animSetId = apiAnimSetId;
    native_.uiSend = apiUiSend;
    native_.uiPoll = apiUiPoll;
    native_.perfStats = apiPerfStats;
    native_.hudSet = apiHudSet;
    native_.waterSample = apiWaterSample;
    native_.waterSplash = apiWaterSplash;
    native_.audioSound = apiAudioSound;
    native_.audioPlay = apiAudioPlay;
    native_.audioImpact = apiAudioImpact;
    native_.audioStop = apiAudioStop;
    native_.audioSet = apiAudioSet;
    native_.audioPlaying = apiAudioPlaying;
    native_.animEvents = apiAnimEvents;
    native_.audioBusVolume = apiAudioBusVolume;
    native_.contactPoll = apiContactPoll;
    native_.surfaceAt = apiSurfaceAt;
    native_.mouseGet = apiMouseGet;
    native_.mouseLock = apiMouseLock;
    native_.entityFind = apiEntityFind;
    native_.entityFindUnder = apiEntityFindUnder;
    native_.animHideBone = apiAnimHideBone;
    native_.animFootIk = apiAnimFootIk;
    native_.animLookAt = apiAnimLookAt;
    native_.animReach = apiAnimReach;
    native_.animAimAt = apiAnimAimAt;
    native_.raycast = apiRaycast;
    native_.prefabSpawn = apiPrefabSpawn;
    native_.particlesControl = apiParticlesControl;
    native_.particlesEmit = apiParticlesEmit;
    native_.particlesCount = apiParticlesCount;
    native_.particlesHits = apiParticlesHits;
    native_.particlesScript = apiParticlesScript;
    native_.decalPlace = apiDecalPlace;
    native_.decalsClear = apiDecalsClear;
    native_.animEntity = apiAnimEntity;
    native_.animCharacter = apiAnimCharacter;
    native_.entityEnabled = apiEntityEnabled;
    native_.entityName = apiEntityName;
    native_.entitySetName = apiEntitySetName;
    native_.entityParent = apiEntityParent;
    native_.entitySetParent = apiEntitySetParent;
    native_.entityChildren = apiEntityChildren;
    native_.entitySetWorld = apiEntitySetWorld;
    native_.renderGet = apiRenderGet;
    native_.renderSet = apiRenderSet;
    native_.renderMaterial = apiRenderMaterial;
    native_.meshSpawn = apiMeshSpawn;
    native_.refInit = apiRefInit;
    native_.refGet = apiRefGet;
    native_.bodyGet = apiBodyGet;
    native_.bodySet = apiBodySet;
    native_.bodyPush = apiBodyPush;
    native_.bodyTeleport = apiBodyTeleport;
    native_.bodyRebuild = apiBodyRebuild;
    native_.physicsGravity = apiPhysicsGravity;
    native_.raycastFiltered = apiRaycastFiltered;
    native_.shapeCast = apiShapeCast;
    native_.overlap = apiOverlap;
    native_.triggerPoll = apiTriggerPoll;
    native_.characterMove = apiCharacterMove;
    native_.jointCreate = apiJointCreate;
    native_.jointDestroy = apiJointDestroy;
    native_.jointMotor = apiJointMotor;
    native_.scriptPoll = apiScriptPoll;
    native_.gamepads = context_.gamepads;
    native_.gamepadCount = 8;
    native_.gamepadRumble = apiGamepadRumble;
    native_.textInput = apiTextInput;
    native_.textMode = apiTextMode;
    native_.timeScale = apiTimeScale;
    native_.quit = apiQuit;
    native_.cameraFov = apiCameraFov;
    native_.renderSetting = apiRenderSetting;
    native_.debugLine = apiDebugLine;
    native_.gizmoLine = apiGizmoLine;
    native_.streamCreate = apiStreamCreate;
    native_.streamWrite = apiStreamWrite;
    native_.streamInfo = apiStreamInfo;
    native_.streamPlay = apiStreamPlay;
    native_.streamDestroy = apiStreamDestroy;
    native_.physicsShapeAdd = apiPhysicsShapeAdd;
    native_.physicsShapesClear = apiPhysicsShapesClear;
    native_.netOpen = apiNetOpen;
    native_.netClose = apiNetClose;
    native_.netListen = apiNetListen;
    native_.netConnect = apiNetConnect;
    native_.netSend = apiNetSend;
    native_.netDisconnect = apiNetDisconnect;
    native_.netPoll = apiNetPoll;
    native_.netStats = apiNetStats;
    native_.netCertificate = apiNetCertificate;
    native_.netLaunch = apiNetLaunch;
    native_.netLaunchClients = apiNetLaunchClients;
    native_.simSetTick = apiSimSetTick;
    native_.simSetRate = apiSimSetRate;
    native_.simFocus = apiSimFocus;
    native_.serverCommand = apiServerCommand;
    native_.steamProc = apiSteamProc;
    native_.steamStatus = apiSteamStatus;
    native_.steamPump = apiSteamPump;
    native_.simWindow = apiSimWindow;
    native_.simMaxTicks = apiSimMaxTicks;
    native_.simRollback = apiSimRollback;
    native_.simPhysicsHash = apiSimPhysicsHash;
    native_.steamNextCallback = apiSteamNextCallback;
    native_.animGetFloat = apiAnimGetFloat;
    native_.animGetId = apiAnimGetId;
    native_.animInState = apiAnimInState;
    native_.animStateName = apiAnimStateName;
    native_.animRate = apiAnimRate;
    native_.animPlay = apiAnimPlay;
    native_.animPlaying = apiAnimPlaying;
    native_.animRootMotion = apiAnimRootMotion;
    native_.terrainSample = apiTerrainSample;
    native_.terrainRaycast = apiTerrainRaycast;
    native_.terrainLayerName = apiTerrainLayerName;
    native_.waterBody = apiWaterBody;
    native_.waterNearestSurface = apiWaterNearestSurface;
    native_.vfxComponent = apiVfxComponent;
    native_.vfxField = apiVfxField;
    native_.uiState = apiUiState;
    native_.materialFind = apiMaterialFind;
    native_.materialParam = apiMaterialParam;
    native_.materialInstance = apiMaterialInstance;
    native_.postCreate = apiPostCreate;
    native_.postSet = apiPostSet;
    native_.postState = apiPostState;
    native_.postDestroy = apiPostDestroy;
    native_.meshCreate = apiMeshCreate;
    fillLevelApi(level_, &context_);
    native_.level = &level_;
    native_.meshUpdate = apiMeshUpdate;
    native_.meshDestroy = apiMeshDestroy;
    native_.meshDraw = apiMeshDraw;
    native_.sceneCurrent = apiSceneCurrent;
    native_.sceneLoad = apiSceneLoad;
    native_.sceneLoadAsync = apiSceneLoadAsync;
    native_.sceneLoading = apiSceneLoading;
    native_.scriptEntityByKey = apiScriptEntityByKey;
    int keyCount = 0;
    sdlKeys_ = reinterpret_cast<const uint8_t*>(SDL_GetKeyboardState(&keyCount));  // bool[]: one byte each
    native_.keys = sdlKeys_;
    native_.keyCount = std::min(keyCount, int(sizeof(keysPrev_)));
    native_.keysPrev = keysPrev_;

    // Runtime start (JIT) or image load (AOT), then Falcon.Engine's Initialize (loads and
    // registers the game): all of it off the main thread.
    loader_ = std::thread([this] {
        loadOk_ = jit_ ? loadJit() : loadAot();
        if (!loadOk_)
            return;
        const uint64_t t0 = SDL_GetTicksNS();
        if (init_(&native_, &managed_, jit_ ? path_.c_str() : nullptr) != 0 || !managed_.start || !managed_.update) {
            FX_ERROR("script: Falcon.Engine initialization failed");
            managed_ = {};
            loadOk_ = false;
            return;
        }
        FX_INFO("script: game code loaded (%.1f ms)", double(SDL_GetTicksNS() - t0) * 1e-6);
    });
    return true;
}

bool ScriptHost::loadAot()
{
    FX_ZONE_CAT("ScriptHost::loadAot", Scripts);
    const uint64_t t0 = SDL_GetTicksNS();
    void* lib = platform::loadLibrary(std::filesystem::path(path_));
    if (!lib) {
        FX_ERROR("script: cannot load %s (%s)", path_.c_str(), platform::lastError().c_str());
        return false;
    }
    library_ = lib;
    init_ = reinterpret_cast<FxScriptInit>(platform::librarySymbol(lib, "falcon_script_init"));
    if (!init_) {
        FX_ERROR("script: %s does not export falcon_script_init (not a Falcon NativeAOT game?)", path_.c_str());
        return false;
    }
    FX_INFO("script: NativeAOT %s loaded (%.1f ms)", path_.c_str(), double(SDL_GetTicksNS() - t0) * 1e-6);
    return true;
}

bool ScriptHost::loadJit()
{
    FX_ZONE_CAT("ScriptHost::loadJit", Scripts);
    const uint64_t t0 = SDL_GetTicksNS();
    // Falcon.Engine is loaded from a shadow copy: a loaded assembly file stays locked, and the
    // game's build writes Falcon.Engine.dll into the same folder on every rebuild.
    const std::filesystem::path dir = std::filesystem::path(path_).parent_path();
    const std::filesystem::path shadow = dir / ".falcon-host";
    std::error_code ec;
    std::filesystem::create_directories(shadow, ec);
    for (const char* file : {"Falcon.Engine.dll", "Falcon.Engine.pdb"})
        if (std::filesystem::exists(dir / file, ec))
            std::filesystem::copy_file(dir / file, shadow / file, std::filesystem::copy_options::overwrite_existing, ec);
    const std::filesystem::path engineDll = shadow / "Falcon.Engine.dll";
    const std::filesystem::path config = std::filesystem::path(path_).replace_extension(".runtimeconfig.json");

    // Deterministic floats: the JIT compiles for the instruction set NativeAOT games ship with
    // (x86-64-v2: no AVX, so no FMA in System.Numerics). Measured: with AVX the same Vector3 code
    // gives other bits, so an editor client would drift from a shipped game (Falcon.Game.targets
    // pins the AOT side). Read once, by the runtime's start below.
    if (!platform::hasEnv("DOTNET_EnableAVX"))
        platform::setEnv("DOTNET_EnableAVX", "0");
    char_t fxrPath[4096];
    size_t fxrSize = std::size(fxrPath);
    if (get_hostfxr_path(fxrPath, &fxrSize, nullptr) != 0) {
        FX_ERROR("script: no .NET runtime found (hostfxr); install .NET 10 or use the NativeAOT build");
        return false;
    }
    void* fxr = platform::loadLibrary(std::filesystem::path(fxrPath));
    if (!fxr)
        return false;
    library_ = fxr;
    auto initFn = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
        platform::librarySymbol(fxr, "hostfxr_initialize_for_runtime_config"));
    auto delegateFn = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(platform::librarySymbol(fxr, "hostfxr_get_runtime_delegate"));
    if (!initFn || !delegateFn)
        return false;
    hostfxr_handle handle = nullptr;
    if (initFn(hostString(config).c_str(), nullptr, &handle) < 0 || !handle) {
        FX_ERROR("script: hostfxr cannot initialize from %s", config.string().c_str());
        return false;
    }
    hostContext_ = handle;
    load_assembly_fn loadAssembly = nullptr;
    get_function_pointer_fn getFunction = nullptr;
    if (delegateFn(handle, hdt_load_assembly, reinterpret_cast<void**>(&loadAssembly)) < 0 ||
        delegateFn(handle, hdt_get_function_pointer, reinterpret_cast<void**>(&getFunction)) < 0)
        return false;
    // Falcon.Engine goes into the default load context (shared by every game reload).
    if (loadAssembly(hostString(engineDll).c_str(), nullptr, nullptr) < 0) {
        FX_ERROR("script: cannot load %s", engineDll.string().c_str());
        return false;
    }
    if (getFunction(FX_HOST_STR("Falcon.Host, Falcon.Engine"), FX_HOST_STR("Initialize"), UNMANAGEDCALLERSONLY_METHOD, nullptr, nullptr,
                    reinterpret_cast<void**>(&init_)) < 0 || !init_) {
        FX_ERROR("script: Falcon.Host.Initialize not found");
        return false;
    }
    FX_INFO("script: CoreCLR up, %s (%.1f ms)", engineDll.string().c_str(), double(SDL_GetTicksNS() - t0) * 1e-6);
    return true;
}

void ScriptHost::addScripts(uint64_t entity, const std::vector<ScriptEntry>& scripts)
{
    if (!active())
        return;  // (no game code: nothing to make them; a game makes the scene's when it starts)
    for (const ScriptEntry& e : scripts)
        context_.pendingScripts.emplace_back(entity, e);
}

void ScriptHost::describeTypes()
{
    gizmoScene_ = "\x01";  // (X3) new code: its gizmo components made again
    editorTools_ = "{}";
    if (managed_.editorDescribe) {
        std::string tools(2048, '\0');
        int32_t n = managed_.editorDescribe(tools.data(), int32_t(tools.size()));
        if (n < 0) {
            tools.resize(size_t(-n));
            n = managed_.editorDescribe(tools.data(), int32_t(tools.size()));
        }
        if (n > 0)
            editorTools_.assign(tools.data(), size_t(n));
    }
    componentTypes_ = "[]";
    if (!managed_.describe)
        return;
    std::string buffer(4096, '\0');
    int32_t n = managed_.describe(buffer.data(), int32_t(buffer.size()));
    if (n < 0) {
        buffer.resize(size_t(-n));
        n = managed_.describe(buffer.data(), int32_t(buffer.size()));
    }
    if (n > 0)
        componentTypes_.assign(buffer.data(), size_t(n));
    // (X2) Its data components too.
    dataTypes_ = "[]";
    if (managed_.dataTypes) {
        std::vector<char> text(64 * 1024);
        int32_t length = managed_.dataTypes(text.data(), int32_t(text.size()));
        if (length < 0) {
            text.resize(size_t(-length));
            length = managed_.dataTypes(text.data(), int32_t(text.size()));
        }
        if (length > 0)
            dataTypes_.assign(text.data(), size_t(length));
    }
}

std::string ScriptHost::componentValues(uint64_t entity)
{
    if (!running_ || !managed_.componentValues)
        return {};
    std::string buffer(2048, '\0');
    int32_t n = managed_.componentValues(entity, buffer.data(), int32_t(buffer.size()));
    if (n < 0) {
        buffer.resize(size_t(-n));
        n = managed_.componentValues(entity, buffer.data(), int32_t(buffer.size()));
    }
    return n > 0 ? std::string(buffer.data(), size_t(n)) : std::string();
}

void ScriptHost::editorUpdate(float dt)
{
    if (active() && managed_.editorUpdate)
        managed_.editorUpdate(dt);
}

bool ScriptHost::editorRun(const std::string& menu)
{
    return active() && managed_.editorRun && managed_.editorRun(menu.c_str()) != 0;
}

void ScriptHost::editorEvent(const std::string& json)
{
    if (active() && managed_.editorEvent)
        managed_.editorEvent(json.c_str(), int32_t(json.size()));
}

void ScriptHost::fixedUpdate(float dt)
{
    if (!running_ || !managed_.fixedUpdate)
        return;
    if (resimTick_ != ~0ull) {  // (a rollback's tick simulated again, inside physics' step: kept after it)
        runTick(dt, resimTick_, 1 | 2);
        return;
    }
    if (context_.nextTick != ~0ull)  // (a client's ticks set ahead of the host's)
        ticks_ = std::exchange(context_.nextTick, ~0ull);
    // The tick's input in the frame's place while it runs (the managed side reads these arrays).
    const uint8_t* frameKeys = native_.keys;
    const uint8_t* framePrev = native_.keysPrev;
    for (int32_t k = 0; k < native_.keyCount; ++k)
        tickKeys_[k] = frameKeys[k] | tickTaps_[k];
    std::memset(tickTaps_, 0, sizeof(tickTaps_));
    native_.keys = tickKeys_;
    native_.keysPrev = tickKeysPrev_;
    FxMouse& m = context_.mouse;
    const FxMouse frameMouse = m;
    m.dx = std::exchange(tickDx_, 0.0f);
    m.dy = std::exchange(tickDy_, 0.0f);
    m.wheel = std::exchange(tickWheel_, 0.0f);
    m.buttons = frameMouse.buttons | std::exchange(tickMouseTaps_, 0u);
    m.buttonsPrev = tickMousePrev_;
    uint32_t framePads[8][2];
    for (int slot = 0; slot < 8; ++slot) {
        FxGamepad& g = context_.gamepads[slot];
        framePads[slot][0] = g.buttons, framePads[slot][1] = g.buttonsPrev;
        g.buttons |= std::exchange(tickPadTaps_[slot], 0u);
        g.buttonsPrev = tickPadPrev_[slot];
    }
    // (kept for a rollback's ticks simulated again)
    TickInput& kept = tickInputs_[ticks_ % kTickInputs];
    std::memcpy(kept.keys, tickKeys_, sizeof(kept.keys));
    kept.mouse = m;
    for (int slot = 0; slot < 8; ++slot) {
        kept.padButtons[slot] = context_.gamepads[slot].buttons;
        std::memcpy(kept.padAxes[slot], context_.gamepads[slot].axes, sizeof(kept.padAxes[slot]));
    }
    managed_.fixedUpdate(dt, ticks_++, 0);
    // This tick's state is the next one's "before"; the frame's comes back.
    std::memcpy(tickKeysPrev_, tickKeys_, sizeof(tickKeysPrev_));
    tickMousePrev_ = m.buttons;
    native_.keys = frameKeys;
    native_.keysPrev = framePrev;
    m.dx = frameMouse.dx, m.dy = frameMouse.dy, m.wheel = frameMouse.wheel;
    m.buttons = frameMouse.buttons, m.buttonsPrev = frameMouse.buttonsPrev;
    for (int slot = 0; slot < 8; ++slot) {
        FxGamepad& g = context_.gamepads[slot];
        tickPadPrev_[slot] = g.buttons;
        g.buttons = framePads[slot][0], g.buttonsPrev = framePads[slot][1];
    }
}

bool ScriptHost::rollback(uint64_t ticksBack)
{
    if (!running_ || !managed_.fixedUpdate || !managed_.simControl || ticksBack == 0 || ticksBack >= kTickInputs - 1 ||
        ticks_ < ticksBack + 1)
        return false;
    const uint64_t last = ticks_ - 1, from = last - ticksBack;
    if (managed_.simControl(0, from) != 1)
        return false;
    if (physicsRollback_)  // (the world too: physics' steps taken again, the scripts' ticks inside them)
        return physicsRollback_(from, last);
    for (uint64_t t = from + 1; t <= last; ++t)
        runTick(1.0f / 60.0f, t, 1);
    return true;
}

// A tick simulated again: its kept input in the frame's place (edges against the tick before's).
void ScriptHost::runTick(float dt, uint64_t tick, int32_t flags)
{
    const TickInput& in = tickInputs_[tick % kTickInputs];
    const TickInput& before = tickInputs_[(tick + kTickInputs - 1) % kTickInputs];
    const uint8_t* frameKeys = native_.keys;
    const uint8_t* framePrev = native_.keysPrev;
    native_.keys = in.keys;
    native_.keysPrev = before.keys;
    FxMouse& m = context_.mouse;
    const FxMouse frameMouse = m;
    m.dx = in.mouse.dx, m.dy = in.mouse.dy, m.wheel = in.mouse.wheel;
    m.buttons = in.mouse.buttons, m.buttonsPrev = in.mouse.buttonsPrev;
    FxGamepad framePads[8];
    std::memcpy(framePads, context_.gamepads, sizeof(framePads));
    for (int slot = 0; slot < 8; ++slot) {
        FxGamepad& g = context_.gamepads[slot];
        g.buttons = in.padButtons[slot];
        g.buttonsPrev = before.padButtons[slot];
        std::memcpy(g.axes, in.padAxes[slot], sizeof(g.axes));
    }
    managed_.fixedUpdate(dt, tick, flags);
    native_.keys = frameKeys;
    native_.keysPrev = framePrev;
    m.dx = frameMouse.dx, m.dy = frameMouse.dy, m.wheel = frameMouse.wheel;
    m.buttons = frameMouse.buttons, m.buttonsPrev = frameMouse.buttonsPrev;
    std::memcpy(context_.gamepads, framePads, sizeof(framePads));
}

std::string ScriptHost::netStatus()
{
    if (!running_ || !managed_.netStatus)
        return {};
    std::string buffer(4096, '\0');
    int32_t n = managed_.netStatus(buffer.data(), int32_t(buffer.size()));
    if (n < 0) {
        buffer.resize(size_t(-n));
        n = managed_.netStatus(buffer.data(), int32_t(buffer.size()));
    }
    return n > 0 ? buffer.substr(0, size_t(n)) : std::string();
}

void ScriptHost::afterStep()
{
    if (!active() || !running_ || !managed_.phase)
        return;
    context_.stepEvents = true;
    managed_.phase(6, 1.0f / 60.0f);
    context_.stepEvents = false;
}

bool ScriptHost::attach(Scene& scene, ScenePak& pak, PhysicsWorld* physics, AnimationSystem* animation,
                        const SkinningSystem* skinning, UiLayer* ui)
{
    FX_ZONE_CAT("ScriptHost::attach", Scripts);
    if (loader_.joinable())
        loader_.join();
    if (!loadOk_ || !managed_.start)
        return false;
    context_.scene = &scene;
    context_.pak = &pak;
    context_.idMeshInstance = scene.world().component<MeshInstance>().id();
    context_.idRigidBody = scene.world().component<RigidBody>().id();
    context_.idParent = scene.world().component<Parent>().id();
    context_.idPhysicsBody = scene.world().component<PhysicsBody>().id();
    describeTypes();
    context_.physics = physics;
    context_.animation = animation;
    context_.skinning = skinning;
    context_.ui = ui;
    return true;
}

bool ScriptHost::start(Scene& scene, ScenePak& pak, PhysicsWorld* physics, AnimationSystem* animation,
                       const SkinningSystem* skinning, UiLayer* ui)
{
    if (!attach(scene, pak, physics, animation, skinning, ui))
        return false;
    const bool ok = startGame();
    context_.tracking = false;  // a game owns its world: nothing to undo
    return ok;
}

bool ScriptHost::startGame()
{
    if (!active() || running_)
        return running_;
    FX_ZONE_CAT("ScriptHost::startGame", Scripts);
    context_.tracking = true;
    context_.created.clear();
    context_.createdSet.clear();
    context_.disabled.clear();
    context_.scriptDisabled.clear();
    std::memset(keysPrev_, 0, sizeof(keysPrev_));
    std::memset(tickKeysPrev_, 0, sizeof(tickKeysPrev_));
    std::memset(tickTaps_, 0, sizeof(tickTaps_));
    tickDx_ = tickDy_ = tickWheel_ = 0.0f;
    tickMouseTaps_ = tickMousePrev_ = 0;
    std::memset(tickPadTaps_, 0, sizeof(tickPadTaps_));
    std::memset(tickPadPrev_, 0, sizeof(tickPadPrev_));
    ticks_ = 0;
    {
        const FxMouse view = context_.mouse;  // (the game's view stays: setView)
        context_.mouse = {};
        context_.mouse.viewX = view.viewX, context_.mouse.viewY = view.viewY;
        context_.mouse.viewWidth = view.viewWidth, context_.mouse.viewHeight = view.viewHeight;
    }
    context_.pendingDx = context_.pendingDy = context_.pendingWheel = 0.0f;
    context_.pendingButtons = context_.pendingPresses = 0;
    context_.mouseKnown = false;
    context_.mouseFreed = false;
    context_.characterRecords.clear();
    context_.pendingScripts.clear();
    context_.uiInbox.clear();
    if (sceneScripts_)
        for (const auto& [entity, list] : sceneScripts_())
            for (const ScriptEntry& e : list)
                context_.pendingScripts.emplace_back(entity, e);
    const uint64_t t0 = SDL_GetTicksNS();
    if (managed_.start() != 0) {
        FX_ERROR("script: the game failed to start");
        stopGame();
        return false;
    }
    running_ = true;
    FX_INFO("script: %s game started (%.1f ms)%s", jit_ ? "JIT" : "NativeAOT", double(SDL_GetTicksNS() - t0) * 1e-6,
            jit_ && hotReload_ ? ", hot reload on" : "");
    return true;
}

void ScriptHost::stopGame()
{
    if (!active())
        return;
    FX_ZONE_CAT("ScriptHost::stopGame", Scripts);
    if (running_ && managed_.stop)
        managed_.stop();
    running_ = false;
    context_.transports.clear();  // (the game's connections end with it)
    setMouseLock(context_, false);
    apiTextMode(&context_, 0);
    context_.timeScale = 1.0f;
    context_.simRate = 1.0f;
    context_.maxTicks = ~0u;
    context_.simFocus.clear();
    context_.quitRequested = false;
    context_.debugLines.clear();
    while (!context_.streams.empty())  // (X4) the game's audio streams
        dropStream(context_, context_.streams.begin()->first);
    if (!context_.renderSaved.empty() && context_.render)  // (the render settings as they were)
        std::memcpy(context_.render, context_.renderSaved.data(), sizeof(FrameSettings));
    context_.renderSaved.clear();
    context_.characterRecords.clear();
    if (!context_.hud.empty()) {
        context_.hud.clear();
        ++context_.hudRevision;
    }
    if (context_.materials)  // (the editor's Stop, a game's end: every material as it was)
        context_.materials->restoreScripted();
    context_.postEffects.clear();  // (and no effect of the game's)
    if (context_.meshes)  // (nor its meshes)
        for (const uint32_t m : context_.gameMeshes)
            context_.meshes->destroy(m);
    context_.gameMeshes.clear();
    levelGameEnded(context_);  // (nor its live meshes)
    if (!context_.tracking)
        return;
    context_.tracking = false;
    ecs_world_t* w = context_.scene->world().c_ptr();
    uint32_t deleted = 0;
    for (const uint64_t e : context_.created)
        if (ecs_is_alive(w, e)) {
            if (context_.physics)
                context_.physics->removeBody(e);
            ecs_delete(w, e);
            ++deleted;
        }
    // What it destroyed or disabled, enabled again (bodies too); the rows it changed as they were.
    for (const uint64_t e : context_.disabled)
        if (ecs_is_alive(w, e))
            setEntityEnabled(context_, e, true);
    for (const uint64_t e : context_.scriptDisabled)
        if (ecs_is_alive(w, e))
            setEntityEnabled(context_, e, true);
    for (const auto& [slot, r] : context_.rows) {
        fpak::InstanceGpu& row = context_.pak->mutableInstances()[slot];
        row.flags = r.original.flags;
        row.tint = r.original.tint;
        context_.touched.push_back(slot);
    }
    context_.rows.clear();
    context_.names.clear();
    context_.pendingScripts.clear();
    if (context_.gravitySet && context_.physics)  // (the world's gravity as it was)
        context_.physics->setGravity({context_.gravity[0], context_.gravity[1], context_.gravity[2]});
    context_.gravitySet = false;
    // Its own components (from any entity): the game's state goes with it. Engine components on
    // the scene's entities are the editor's to restore.
    for (const uint64_t id : context_.components)
        ecs_remove_all(w, id);
    FX_INFO("script: game stopped (%u entities it made deleted, %zu brought back)", deleted, context_.disabled.size());
    context_.created.clear();
    context_.createdSet.clear();
    context_.disabled.clear();
    context_.scriptDisabled.clear();
}

void ScriptHost::phase(Phase phase, float dt)
{
    if (!active() || !running_ || !managed_.phase)
        return;
    FX_ZONE_CAT("ScriptHost::phase", Scripts);
    managed_.phase(int32_t(phase), dt);
}

void ScriptHost::unloadScene()
{
    if (!active() || !running_ || !managed_.sceneUnloading)
        return;
    FX_ZONE_CAT("ScriptHost::unloadScene", Scripts);
    managed_.sceneUnloading(context_.scenePath.c_str());
}

bool ScriptHost::loadScene(Scene& scene, ScenePak& pak, PhysicsWorld* physics, AnimationSystem* animation, const SkinningSystem* skinning,
                           UiLayer* ui)
{
    if (!running_)
        return start(scene, pak, physics, animation, skinning, ui);
    if (!attach(scene, pak, physics, animation, skinning, ui))
        return false;
    continueGame();
    return true;
}

void ScriptHost::continueGame()
{
    FX_ZONE_CAT("ScriptHost::continueGame", Scripts);
    context_.tracking = false;
    context_.pendingScripts.clear();
    if (sceneScripts_)
        for (const auto& [entity, list] : sceneScripts_())
            for (const ScriptEntry& e : list)
                context_.pendingScripts.emplace_back(entity, e);
    const uint64_t t0 = SDL_GetTicksNS();
    if (managed_.sceneLoaded && managed_.sceneLoaded(context_.scenePath.c_str()) != 0)
        FX_ERROR("script: the game's OnSceneLoaded failed (it goes on)");
    FX_INFO("script: the game goes on in %s (%.1f ms)", context_.scenePath.empty() ? "the next scene" : context_.scenePath.c_str(),
            double(SDL_GetTicksNS() - t0) * 1e-6);
}

std::vector<PostProcess::EffectRequest> ScriptHost::postEffects() const
{
    std::vector<std::pair<std::pair<int32_t, int32_t>, const Context::PostEffect*>> on;
    for (const auto& [id, e] : context_.postEffects)
        if (e.enabled)
            on.push_back({{e.order, int32_t(id)}, &e});
    std::sort(on.begin(), on.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<PostProcess::EffectRequest> out;
    for (const auto& [key, e] : on)
        out.push_back({e->blob, e->stage == 1, e->params});
    return out;
}

void ScriptHost::detachScene()
{
    Context& x = context_;
    x.scene = nullptr;
    x.pak = nullptr;
    x.idMeshInstance = x.idRigidBody = x.idParent = x.idPhysicsBody = 0;
    x.gravitySet = false;
    x.physics = nullptr;
    x.animation = nullptr;
    x.skinning = nullptr;
    x.water = nullptr;
    x.terrain = nullptr;
    x.materials = nullptr;
    x.particles = nullptr;
    x.prefabs = nullptr;
    x.runtimeMeshes = nullptr;
    x.placeDecal = {};
    x.clearDecals = {};
    x.tracking = false;
    x.created.clear();
    x.createdSet.clear();
    x.disabled.clear();
    x.components.clear();
    x.findName = {};
    x.findNameUnder = {};
    x.nameOf = {};
    x.entityByKey = {};
    x.uiSend = {};
    x.uiInbox.clear();
    x.pendingScripts.clear();
    x.names.clear();
    x.scriptDisabled.clear();
    x.rows.clear();
    x.touched.clear();
    x.debugLines.clear();
    x.characterRecords.clear();
    sceneScripts_ = {};
    native_.editor = nullptr;
    testKeys_ = nullptr;
}

void ScriptHost::extract(std::vector<InstanceUpdate>& rows)
{
    Context& x = context_;
    levelUpdate(x);  // (live meshes whose entity went: freed, their rows hidden)
    if (x.touched.empty() || !x.pak)
        return;
    std::sort(x.touched.begin(), x.touched.end());
    x.touched.erase(std::unique(x.touched.begin(), x.touched.end()), x.touched.end());
    for (const uint32_t slot : x.touched) {
        fpak::InstanceGpu& row = x.pak->mutableInstances()[slot];
        if (const flecs::entity e = x.scene->entity(slot); e && e.is_alive())
            if (const WorldTransform* t = e.try_get<WorldTransform>())
                std::memcpy(row.world, t->rows, sizeof(row.world));
        rows.push_back({slot, row});
    }
    x.touched.clear();
}

void ScriptHost::frameTime(float ms)
{
    Context& x = context_;
    x.frameMs = ms;
    x.windowMs += ms;
    x.windowMax = std::max(x.windowMax, double(ms));
    ++x.windowFrames;
    if (x.windowMs >= 500.0) {
        x.frameMsAvg = float(x.windowMs / x.windowFrames);
        x.frameMsMax = float(x.windowMax);
        x.fps = float(1000.0 * x.windowFrames / x.windowMs);
        x.windowMs = x.windowMax = 0.0;
        x.windowFrames = 0;
    }
}

uint32_t ScriptHost::gameEntities() const
{
    if (!context_.tracking || !context_.scene)
        return 0;
    ecs_world_t* w = context_.scene->world().c_ptr();
    uint32_t n = 0;
    for (const uint64_t e : context_.created)  // (bodies it spawned are counted as bodies)
        n += ecs_is_alive(w, e) && !ecs_has_id(w, e, context_.idRigidBody) ? 1 : 0;
    return n;
}

void ScriptHost::particleStep(uint64_t script, const char* className, void* particles, int32_t count, const void* hits, int32_t hitCount,
                              float dt)
{
    if (!active() || !running_ || !managed_.particleStep)
        return;
    FX_ZONE_CAT("ScriptHost::particleStep", Scripts);
    managed_.particleStep(script, className, particles, count, static_cast<const FxParticleHit*>(hits), hitCount, dt);
}

void ScriptHost::update(float dt, double time, uint64_t frame, const CameraState& camera)
{
    if (!active() || !running_)
        return;
    FX_ZONE_CAT("ScriptHost::update", Scripts);
    context_.camera = camera;
    if (!context_.dyingStreams.empty())
        reapStreams(context_);

    // Hot reload: poll the file's timestamp 4x per second; reload once it stopped changing.
    if (jit_ && hotReload_) {
        const uint64_t now = SDL_GetTicksNS();
        if (now - lastPollNs_ > 250'000'000ull) {
            lastPollNs_ = now;
            std::error_code ec;
            const auto stamp = std::filesystem::last_write_time(path_, ec);
            if (!ec && stamp != stamp_) {
                if (++pendingChange_ >= 2) {
                    stamp_ = stamp;
                    pendingChange_ = 0;
                    reload();
                }
            } else {
                pendingChange_ = 0;
            }
        }
    }

    FxMouse& m = context_.mouse;
    m.dx = std::exchange(context_.pendingDx, 0.0f);
    m.dy = std::exchange(context_.pendingDy, 0.0f);
    m.wheel = std::exchange(context_.pendingWheel, 0.0f);
    m.buttonsPrev = m.buttons;
    m.buttons = context_.pendingButtons | std::exchange(context_.pendingPresses, 0u);  // (a click within a frame: down for it)
    if (testKeys_) {  // the keyboard and what a test holds
        for (int32_t k = 0; k < native_.keyCount; ++k)
            keys_[k] = sdlKeys_[k] | testKeys_[k];
        native_.keys = keys_;
    }
    if (SDL_SCANCODE_ESCAPE < native_.keyCount && native_.keys[SDL_SCANCODE_ESCAPE] && !keysPrev_[SDL_SCANCODE_ESCAPE])
        freeMouse();  // (the editor's page may take the key's event: its state still shows it)
    for (int slot = 0; slot < 8; ++slot)
        if (auto* pad = static_cast<SDL_Gamepad*>(context_.gamepadHandles[slot])) {
            FxGamepad& g = context_.gamepads[slot];
            g.buttonsPrev = g.buttons;
            g.buttons = 0;
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; ++b)
                g.buttons |= SDL_GetGamepadButton(pad, SDL_GamepadButton(b)) ? 1u << b : 0u;
            for (int a = 0; a < 6; ++a) {
                const float v = float(SDL_GetGamepadAxis(pad, SDL_GamepadAxis(a))) / 32767.0f;
                g.axes[a] = std::clamp(v, -1.0f, 1.0f);
            }
        }
    if (inputHook_) {  // (a recording reads the frame's input; a replay puts its own in)
        if (native_.keys != keys_) {
            std::memcpy(keys_, native_.keys, size_t(native_.keyCount));
            native_.keys = keys_;
        }
        FrameInput in = {keys_, native_.keyCount, &context_.mouse, context_.gamepads, &context_.text};
        inputHook_(in);
    }
    // What the ticks after this frame see: taps and motion since the last tick.
    for (int32_t k = 0; k < native_.keyCount; ++k)
        tickTaps_[k] |= native_.keys[k] & ~keysPrev_[k];
    tickDx_ += m.dx, tickDy_ += m.dy, tickWheel_ += m.wheel;
    tickMouseTaps_ |= m.buttons & ~m.buttonsPrev;
    for (int slot = 0; slot < 8; ++slot)
        tickPadTaps_[slot] |= context_.gamepads[slot].buttons & ~context_.gamepads[slot].buttonsPrev;
    native_.realDelta = dt;
    managed_.update(dt * context_.timeScale, time, frame);
    context_.text.clear();
    std::memcpy(keysPrev_, native_.keys, size_t(native_.keyCount));
}

void ScriptHost::event(const SDL_Event& e)
{
    if (!running_)
        return;
    Context& x = context_;
    const auto bit = [](uint8_t button) {
        return button == SDL_BUTTON_LEFT ? 1u : button == SDL_BUTTON_RIGHT ? 2u : button == SDL_BUTTON_MIDDLE ? 4u : 0u;
    };
    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:
        // (Events pushed without relative motion, a test's: from where it was.)
        if (e.motion.xrel != 0.0f || e.motion.yrel != 0.0f) {
            x.pendingDx += e.motion.xrel;
            x.pendingDy += e.motion.yrel;
        } else if (x.mouseKnown) {
            x.pendingDx += e.motion.x - x.mouse.x;
            x.pendingDy += e.motion.y - x.mouse.y;
        }
        x.mouseKnown = true;
        x.mouse.x = e.motion.x;
        x.mouse.y = e.motion.y;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        x.pendingWheel += e.wheel.y;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        x.mouseFreed = false;  // (a click in the game's view: it may hold the cursor again)
        x.pendingButtons |= bit(e.button.button);
        x.pendingPresses |= bit(e.button.button);
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        x.pendingButtons &= ~bit(e.button.button);
        break;
    case SDL_EVENT_TEXT_INPUT:
        if (x.textOn && e.text.text)
            x.text += e.text.text;
        break;
    default:
        break;
    }
}

void ScriptHost::gamepadEvent(const SDL_Event& e)
{
    Context& x = context_;
    if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
        for (int slot = 0; slot < 8; ++slot)
            if (x.gamepads[slot].id == e.gdevice.which)
                return;  // (already open)
        for (int slot = 0; slot < 8; ++slot)
            if (!x.gamepadHandles[slot]) {
                SDL_Gamepad* pad = SDL_OpenGamepad(e.gdevice.which);
                if (!pad)
                    return;
                x.gamepadHandles[slot] = pad;
                x.gamepads[slot] = {};
                x.gamepads[slot].id = e.gdevice.which;
                x.gamepads[slot].type = uint32_t(SDL_GetGamepadType(pad));
                FX_INFO("script: gamepad %d: %s", slot, SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "?");
                return;
            }
    } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
        for (int slot = 0; slot < 8; ++slot)
            if (x.gamepads[slot].id == e.gdevice.which) {
                SDL_CloseGamepad(static_cast<SDL_Gamepad*>(x.gamepadHandles[slot]));
                x.gamepadHandles[slot] = nullptr;
                x.gamepads[slot] = {};
            }
    }
}

void ScriptHost::drawGizmos(uint64_t selected)
{
    context_.gizmoLines.clear();
    if (!active() || !managed_.drawGizmos)
        return;
    FX_ZONE_CAT("ScriptHost::drawGizmos", Scripts);
    // While editing: the scene's script components, passed when they changed.
    std::string scene;
    if (!running_ && sceneScripts_) {
        JsonWriter w;
        w.beginArray();
        for (const auto& [entity, list] : sceneScripts_())
            for (const ScriptEntry& e : list)
                w.beginObject().value("entity", entity).value("class", e.className).value("enabled", e.enabled)
                    .raw("properties", e.properties.empty() ? std::string("{}") : e.properties).endObject();
        w.endArray();
        scene = w.str();
    }
    const bool changed = scene != gizmoScene_;
    if (changed)
        gizmoScene_ = scene;
    managed_.drawGizmos(changed ? gizmoScene_.c_str() : nullptr, selected);
}

std::string ScriptHost::componentButton(uint64_t entity, const std::string& className, const std::string& method,
                                        const std::string& properties)
{
    if (!active() || !managed_.componentButton)
        return {};
    std::vector<char> text(16 * 1024);
    const char* props = properties.empty() ? "{}" : properties.c_str();
    int32_t length = managed_.componentButton(entity, className.c_str(), method.c_str(), props, text.data(), int32_t(text.size()));
    if (length < 0) {
        text.resize(size_t(-length));
        length = managed_.componentButton(entity, className.c_str(), method.c_str(), props, text.data(), int32_t(text.size()));
    }
    return length > 0 ? std::string(text.data(), size_t(length)) : std::string();
}

void ScriptHost::ageDebugLines(float dt)
{
    std::erase_if(context_.debugLines, [&](DebugLine& l) { return (l.left -= dt) <= 0.0f; });
}

void ScriptHost::setView(float x, float y, float width, float height)
{
    context_.mouse.viewX = x;
    context_.mouse.viewY = y;
    context_.mouse.viewWidth = width;
    context_.mouse.viewHeight = height;
}

bool ScriptHost::freeMouse()
{
    if (!running_ || !context_.mouse.locked)
        return false;
    setMouseLock(context_, false);
    context_.mouseFreed = true;
    FX_INFO("script: Esc: the cursor is free (click in the game to hand it back)");
    return true;
}

bool ScriptHost::reload()
{
    if (!active() || !jit_ || !managed_.reload)
        return false;
    FX_ZONE_CAT("ScriptHost::reload", Scripts);
    const uint64_t t0 = SDL_GetTicksNS();
    std::error_code ec;
    stamp_ = std::filesystem::last_write_time(path_, ec);  // (the poll would see this build again)
    pendingChange_ = 0;
    const int32_t r = managed_.reload(path_.c_str());
    const double ms = double(SDL_GetTicksNS() - t0) * 1e-6;
    if (r != 0 && running_ && r != 2)
        running_ = false;  // the new code did not load or start: nothing runs until the next build
    describeTypes();
    if (r == 0)
        FX_INFO("script: hot reload of %s done (%.1f ms)", path_.c_str(), ms);
    else
        FX_ERROR("script: hot reload of %s failed (%s)", path_.c_str(),
                 r == 2 ? "the previous code is still referenced and could not be unloaded" : "load error");
    return r == 0;
}

void ScriptHost::shutdown()
{
    if (loader_.joinable())
        loader_.join();
    if (managed_.stop)
        managed_.stop();
    managed_ = {};
    level::setScriptModifierHandler(nullptr);  // (the C# modifiers gone with the code)
    running_ = false;
    systemstats::shutdown();
    // The CLR cannot be unloaded from a process; hostfxr/AOT libraries stay loaded until exit.
}

}  // namespace fx
