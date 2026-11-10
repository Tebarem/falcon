#pragma once

#include "anim/anim_core.h"
#include "anim/anim_events.h"
#include "anim/anim_graph.h"

#include <flecs.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fx {

class ScenePak;
class SkinningSystem;

// M8: drives every skinned instance. Each one owns a pose evaluated from an animation graph
// (or, without one, a looping clip); per frame the system turns poses into skinning matrices
// and animated bounds for SkinningSystem. Characters update in parallel on the job system.
// A skinned instance plays its skeleton's first cooked graph (or loops its first clip) unless an
// Animator binds it: then one graph instance drives every skinned mesh of the Animator's entity
// and its children (followers take the leader's pose). Hidden instances (unused pool slots) are
// neither evaluated nor skinned.
class AnimationSystem {
public:
    bool init(const anim::AnimData& data, const ScenePak& pak, SkinningSystem& skinning);
    void shutdown();

    // Plays a looping clip on a skinned instance (by skinning record) instead of its graph;
    // clip -1 = reference pose.
    void playClip(uint32_t record, int32_t clip, float speed = 1.0f);
    // Scripts: a clip (by name, of the character's skeleton) played over its graph (or clip), fading
    // in over `fade` seconds, at `speed`, looping or once (then fading out by itself); the graph keeps
    // playing beneath. An empty name fades the clip out. False when there is no such clip. reset()
    // ends it. playing: seconds into the clip (-1: none, or fading out).
    bool play(uint32_t record, std::string_view clip, float speed, float fade, bool loop);
    float playing(uint32_t record) const;
    // Scripts: how fast a character's animation runs (its graph or clip, 1: as authored; 0 holds it);
    // reset() puts it back to 1.
    void setRate(uint32_t record, float rate);
    float rate(uint32_t record) const;
    // The character's graph (nullptr without one): set control parameters on it. A follower's is
    // its leader's.
    anim::GraphInstance* graph(uint32_t record);
    // Root motion of the latest update (character space).
    anim::Xform rootMotion(uint32_t record) const;
    // One step. `editing` (the editor's edit mode): only characters previewing in the editor
    // (setEditorPreview) advance; the others hold their pose (IK still follows its targets).
    void update(float dt, bool editing = false);
    // An Animator's Preview in Editor: its character plays in the editor's edit mode too (its graph's
    // time, IK smoothing). reset() (Play, Stop) starts it again from the start.
    void setEditorPreview(uint32_t record, bool preview);
    bool editorPreview(uint32_t record) const;
    float playedTime(uint32_t record) const;  // seconds its graph (or clip) has played since it started

    // An Animator: `records` (the first leads) play `graph` with `params` (name, value: bools as
    // 0/1) over its defaults. A record of another skeleton than the first's is left out. A null
    // graph puts the records back on their defaults. Returns the records bound.
    using Params = std::vector<std::pair<std::string, float>>;
    uint32_t bind(const std::vector<uint32_t>& records, std::shared_ptr<const anim::GraphDefinition> graph, const Params& params);
    // The skinned records of `root` and its descendants (Parent links), `root`'s own first.
    std::vector<uint32_t> recordsUnder(flecs::world& world, flecs::entity_t root) const;
    // Every character back to its start (graphs rebuilt, clips at 0): the editor's Stop.
    void reset();
    // The characters playing `old` play `now` instead (a graph edited and compiled again), their
    // parameter values kept by name. Returns how many.
    uint32_t replaceGraph(const anim::GraphDefinition* old, std::shared_ptr<const anim::GraphDefinition> now);
    // Clip markup (anim/anim_events.h) onto the clips of `skeleton` (~0u: every skeleton) by name
    // (`replace`: the skeleton's other clips lose theirs); every character starts again (graphs
    // read it when they are instantiated). Returns the clips marked up.
    uint32_t applyMarkup(const anim::Markup& markup, uint32_t skeleton, bool replace = false);
    // A project's: each model's .fevents beside it, for the model's skeleton.
    void loadProjectMarkup(const std::string& projectDir);
    // The skeleton of a model of the scene (its library source): -1 when it has no skinned mesh.
    int32_t modelSkeleton(const std::string& model) const;
    // The markup as read (names kept, for the editor): by model.
    const std::map<std::string, std::string>& markupText() const { return markupText_; }
    void setMarkupText(const std::string& model, std::string text) { markupText_[model] = std::move(text); }
    // The events of a character's last update (a follower's: its leader's).
    const std::vector<anim::SampledEvent>* events(uint32_t record) const;
    // A character's pose of its last update (a follower's: its leader's; parent-space locals and
    // model space); nullptr for no character.
    const anim::Pose* pose(uint32_t record) const;
    // Hides (or shows again) a bone of a character and everything below it (a follower's: its
    // leader's, so every part of the character): first person's head. To the eye, the vertices on
    // hidden bones collapse into the hidden bone's joint (SkinningSystem::collapse); the shadows
    // still have the whole character. False when the character has no such bone. reset() shows
    // every bone again.
    bool hideBone(uint32_t record, std::string_view bone, bool hidden);
    bool boneHidden(uint32_t record, std::string_view bone) const;

    // Inverse kinematics, after the graph's pose (anim/anim_ik.h), on a character (a follower's:
    // its leader's): the feet onto the ground, then a look, then reaches. Targets are in the world.
    // Foot IK: each foot's ground (a ray down at it) lifts or lowers the foot as far as its reach
    // (metres), the hips lower as far as the lower foot needs, the feet tilt to the ground's slope
    // as they plant; smoothed. The legs are found by name (foot_l / LeftFoot, its calf and thigh).
    //
    // A character's IK is its graph's "ik" section (anim/anim_ik_settings.h: feet, look, aim and
    // reaches, their targets the graph's entity parameters, their weights its parameters), solved
    // each frame; else its Animator's own (an older scene's Animator fields, setOwnIk), whose targets
    // name the Animator's bindings. A script's call (lookAt, aimAt, reach, setFootIkWeight) takes
    // that part over until reset().
    void setOwnIk(uint32_t record, std::shared_ptr<const anim::IkSettings> ik);
    const anim::IkSettings* ikSettings(uint32_t record) const;  // what it solves (null: none)
    // Entity parameters: an Animator's bindings (name -> entity, replacing them), a script's (over the
    // Animator's until reset() or resetEntity), and what one is now (0: none). setEntity and
    // resetEntity are false for a name that is neither an entity parameter of the graph nor bound.
    void bindEntities(uint32_t record, const std::vector<std::pair<std::string, uint64_t>>& entities);
    bool setEntity(uint32_t record, std::string_view name, uint64_t entity);
    bool resetEntity(uint32_t record, std::string_view name);
    uint64_t entity(uint32_t record, std::string_view name) const;
    // A script's: how much of the foot IK (0 off: in the air), over the graph's. False when the
    // character has no legs to find.
    bool setFootIkWeight(uint32_t record, float weight);
    float footIkWeight(uint32_t record) const;
    // Looking at `target` (weight 0: not): the bones from the spine up to `tip` (the head) share the
    // turn (anim::lookChain), at most 75 degrees; the tip's forward is the model's +Z at rest.
    bool lookAt(uint32_t record, float3 target, float weight, std::string_view tip = "head");
    // `bone` (a hand) reaching `target` (weight 0: not), its limb its parent and grandparent; the
    // bone keeps its turn. Up to 4 at once; false when there is no such bone (or no room).
    bool reach(uint32_t record, std::string_view bone, float3 target, float weight);
    // Aiming `bone` (a hand holding a weapon; weight 0: not) at `target`: the spine it hangs from
    // (anim::aimChain) turns until the bone points at it, at most 80 degrees, before any look (which
    // then turns the neck and head only). Where the bone points: `axis`, a direction in the bone's
    // own space (a weapon's barrel); without one (zero), ahead (the model's +Z) in the graph's pose,
    // so a clip aiming ahead aims at the target. With `hold` (world), the bone is brought there by
    // its arm after the look (the weapon before the eyes), still pointing at the target, and
    // `support` (the other hand; empty or unknown: none) keeps its grip on it. False when there is
    // no such bone or no spine above it.
    bool aimAt(uint32_t record, float3 target, float weight, std::string_view bone = "hand_r", float3 axis = {},
               const float3* hold = nullptr, std::string_view support = {});
    // The bone aiming and its forward in its own space (as last solved); false when not aiming.
    bool aiming(uint32_t record, uint32_t& bone, float3& forward) const;
    // Where a target entity is (its world transform), false when it is gone or disabled.
    using EntityAt = std::function<bool(uint64_t entity, float world[3][4])>;
    void setEntityWorld(EntityAt entityAt) { entityAt_ = std::move(entityAt); }
    // What IK needs from the world: the ground (a ray: from, translation -> point, normal) and a
    // record's world transform (its instance's entity, as moved this frame).
    using GroundRay = std::function<bool(float3 from, float3 translation, float3& point, float3& normal)>;
    using WorldOf = std::function<bool(uint32_t record, float world[3][4])>;
    void setWorld(GroundRay ground, WorldOf worldOf) { ground_ = std::move(ground), worldOf_ = std::move(worldOf); }
    bool ikActive() const;  // any character with IK on (the engine brings transforms up to date first)

    // A pose of the skeleton onto a skinning record of `mesh` (the Animator's preview, on a floating
    // record): its matrices and animated bounds, returned (model space; shadows not invalidated).
    float4 skinPose(uint32_t record, uint32_t mesh, const anim::Pose& pose);

    uint32_t characterCount() const { return uint32_t(characters_.size()); }
    const anim::AnimData& data() const { return *data_; }
    const anim::Clip* clip(uint32_t index) const { return index < clips_.size() ? &clips_[index] : nullptr; }
    const std::vector<anim::Clip>& clips() const { return clips_; }
    const anim::Skeleton* skeleton(uint32_t index) const { return &skeletons_[index]; }
    float lastUpdateMs() const { return updateMs_; }

private:
    struct Character {
        uint32_t record = 0;
        const anim::Skeleton* skeleton = nullptr;
        anim::Pose pose;
        std::unique_ptr<anim::GraphInstance> graph;
        std::shared_ptr<const anim::GraphDefinition> definition;  // an Animator's (null: the default)
        Params params;
        anim::Xform rootMotion;
        int32_t clip = -1;
        float speed = 1.0f, time = 0.0f;  // clip playback (no graph)
        float rate = 1.0f;                // a script's speed for all of it
        struct Play {                     // a script's clip over the graph
            int32_t clip = -1;
            float speed = 1.0f, time = 0.0f, fade = 0.2f, weight = 0.0f;
            bool loop = true, stopping = false;
            anim::Pose pose;
        } play;
        const std::vector<float>* boneRadii = nullptr;
        int32_t leader = -1;             // a follower: the record whose pose it takes
        std::vector<uint32_t> followers;
        bool active = true;              // its instance (or a follower's) is shown
        bool preview = false;            // plays in the editor's edit mode
        float played = 0.0f;             // seconds played since start()
        std::vector<int32_t> hidden;     // bones hidden (hideBone), with what is below them
        std::vector<int32_t> collapse;   // per bone: the hidden bone it collapses into, -1 shown (empty: none hidden)
        std::shared_ptr<const anim::IkSettings> ownIk;  // its Animator's (used when the graph has none)
        struct EntitySlot {
            std::string name;
            uint64_t bound = 0, scripted = 0;  // the Animator's; a script's (when isScripted)
            bool isScripted = false;
            uint64_t value() const { return isScripted ? scripted : bound; }
        };
        std::vector<EntitySlot> entities;
        struct Reach {
            uint32_t bone = 0;
            float3 target = {};
            float weight = 0.0f;
        };
        struct Ik {
            const anim::IkSettings* settings = nullptr;  // the graph's or the Animator's own (null: none)
            float feetWeight = 0.0f, feetReach = 0.45f;
            bool feetScripted = false;
            int32_t thigh[2] = {-1, -1}, calf[2] = {-1, -1}, foot[2] = {-1, -1}, pelvis = -1;  // found once (-2: none)
            float footOffset[2] = {}, pelvisOffset = 0.0f;  // smoothed (metres, up)
            float3 footNormal[2] = {{0, 1, 0}, {0, 1, 0}};
            bool settled = false;  // the smoothing has a start
            float lookWeight = 0.0f;
            float3 lookTarget = {};
            std::vector<uint32_t> lookChain;
            std::vector<float> lookShares;
            float3 lookForward = {0, 0, 1};
            std::vector<Reach> reaches;
            float aimWeight = 0.0f;
            float3 aimTarget = {};
            int32_t aimBone = -1;
            std::vector<uint32_t> aimChain;
            float3 aimAxis = {};            // a script's, in the bone's space (zero: ahead in the graph's pose)
            bool aimHeld = false;           // the bone brought to aimHold (world), aimSupport keeping its grip
            float aimSpine = 1.0f;          // how much the spine turns to aim (0: the arms only, from the hold)
            float3 aimHold = {};
            int32_t aimSupport = -1;
            float3 aimForward = {0, 0, 1};  // as last solved, in the bone's space
            bool lookScripted = false, aimScripted = false;  // a script took them (until reset)
            std::vector<uint32_t> scriptedReaches;           // bones a script reached
            // This frame's, from the main thread (prepareIk) for the solve on a job thread.
            bool hasWorld = false, feetNow = false;
            float world[3][4] = {};
            float3 ankle[2] = {};
            float height[2] = {};
            std::vector<float> shares;  // the look's this frame (scratch)
            bool active() const
            {
                return feetWeight > 0.0f || lookWeight > 0.0f || aimWeight > 0.0f || !reaches.empty() || (settings && settings->any());
            }
        } ik;
    };

    void start(Character& c);     // graph (or clip) from its definition or the default
    void useIk(Character& c);     // the IK settings it solves (its graph's, else its own)
    void detach(uint32_t record);  // out of its group, back on the default
    void evaluate(Character& c, float dt);  // the pose (skinned now unless IK is to change it)
    void overlay(Character& c, float dt);   // a script's clip over the pose
    void prepareIk(Character& c, float dt);  // the main thread's part: targets, the world, the feet's rays
    void applyIk(Character& c, float dt);    // the solves (any thread: no world reads), then skinned
    void writeSkins(Character& c);           // the leader's matrices, copied to its followers
    void followIk(Character& c);            // the IK settings' parts with their targets where they are now
    const Character::EntitySlot* slot(const Character& c, std::string_view name) const;
    bool findLegs(Character& c);
    Character& leaderOf(uint32_t record);  // the character whose pose a record takes
    const Character& leaderOf(uint32_t record) const;
    void writeSkin(Character& c, uint32_t record, const float* shared = nullptr);  // shared: a record's matrices of the same pose
    float4 writeMatrices(uint32_t record, const anim::Pose& pose, const std::vector<float>& radii);
    static float4 boundsOf(const anim::Pose& pose, const std::vector<float>& radii);  // a mesh's animated bounds

    const anim::AnimData* data_ = nullptr;
    const ScenePak* pak_ = nullptr;
    SkinningSystem* skinning_ = nullptr;
    std::vector<anim::Skeleton> skeletons_;
    std::vector<anim::Clip> clips_;
    std::vector<Character> characters_;  // by record
    std::vector<std::vector<float>> meshBoneRadii_;  // per mesh: max distance of a weighted vertex per bone
    float updateMs_ = 0.0f;
    std::map<std::string, std::string> markupText_;
    GroundRay ground_;
    WorldOf worldOf_;
    EntityAt entityAt_;
    bool followingTargets_ = false;  // the IK settings are being applied (not a script's call)
};

}  // namespace fx
