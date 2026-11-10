#pragma once

// Animation graph runtime, ported from Esoterica (MIT, Engine/Animation/Graph): node
// definitions are a flat table (anim_format.h GraphNode, compiled from JSON by the cooker);
// a GraphInstance builds every node object into one memory block, initializes nodes lazily
// (refcounted, parent-owned), evaluates value nodes at most once per update and lets pose
// nodes register tasks that TaskSystem runs afterwards.
//
// Ported node set (phase 1): parameters, constants, And/Or/Not, float compare/range/math/
// remap/clamp, ID compare, state-completed and time conditions; clip, Blend1D, SpeedScale,
// selector, reference pose, state machine, state and transition (synchronized or not,
// clamped, source-time/sync-event matching, sync offsets). Not yet: layers and bone masks,
// sampled/graph events, cached-pose (forced) transitions, Blend2D, referenced graphs.

#include "anim/anim_core.h"
#include "anim/anim_ik_settings.h"
#include "anim/anim_sync.h"
#include "anim/anim_tasks.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fx::anim {

class GraphInstance;
struct CompiledGraph;

// A graph's definition: its node table, pool and parameter names (parameters are nodes
// [0, parameters.size())). Read in place from the pak's Animation section, or owned by a graph
// compiled at runtime (anim_graph_compiler.h), which instances keep alive while they use it.
struct GraphDefinition {
    std::string name;
    uint32_t skeleton = 0;
    const fmt::GraphNode* nodes = nullptr;
    uint32_t nodeCount = 0;
    const uint32_t* pool = nullptr;
    int16_t root = -1;
    std::vector<std::string> parameters;
    std::vector<std::string> entityParameters;  // entity references (not nodes): the IK's targets
    IkSettings ik;                              // its "ik" section (a pak's graph: none)
    std::shared_ptr<const CompiledGraph> compiled;  // null: the pak's (nodeIds unknown)

    static std::shared_ptr<const GraphDefinition> fromPak(const AnimData& data, uint32_t graph);
    static std::shared_ptr<const GraphDefinition> fromCompiled(std::shared_ptr<const CompiledGraph> graph);
};

// An event of this update: a clip's (sampled between its last and current time) or a state's,
// weighted by how much its branch contributes (blends, transitions).
struct SampledEvent {
    enum Kind : uint8_t { Marker, Foot, State };
    enum Flags : uint8_t { Left = 1, Right = 2, Entry = 4, Exit = 8, FullyIn = 16 };
    uint32_t id = 0;
    float weight = 1.0f;
    float percent = 1.0f;  // how far through it (events with a duration)
    uint8_t kind = Marker, flags = 0;
    bool active = true;    // from the active branch (not a transition's source)
};

struct GraphContext {
    float deltaTime = 0.0f;
    uint32_t updateId = 0;
    bool activeBranch = true;  // false while updating a transition's source
    TaskSystem* tasks = nullptr;
    std::vector<SampledEvent>* events = nullptr;  // this update's, in the order sampled
    // Weights of the events sampled since `from` scaled (a blend's or a transition's inputs).
    void weightEvents(size_t from, float w) const
    {
        for (size_t i = from; i < events->size(); ++i)
            (*events)[i].weight *= w;
    }
};

struct PoseResult {
    int8_t task = TaskSystem::kInvalid;
    Xform rootMotion;  // character-space delta of this update
    bool hasTask() const { return task != TaskSystem::kInvalid; }
};

class Node {
public:
    virtual ~Node() = default;
    void initialize(GraphContext& ctx);
    void shutdown(GraphContext& ctx);
    bool isInitialized() const { return initCount_ > 0; }
    virtual bool isValid() const { return true; }
    virtual void link(GraphInstance&) {}  // resolve node indices (all nodes constructed)
    // Debugging: the node it plays now (a state machine: its active state's), or null; how far
    // through it is (a transition: [0, 1]; -1: not one); updated by the last evaluate.
    virtual const fmt::GraphNode* debugActive() const { return nullptr; }
    virtual float debugProgress() const { return -1.0f; }
    bool debugUpdated(uint32_t updateId) const { return lastUpdate_ == updateId; }

    const fmt::GraphNode* def = nullptr;

protected:
    virtual void initializeInternal(GraphContext& ctx);
    virtual void shutdownInternal(GraphContext& ctx);
    bool wasUpdated(const GraphContext& ctx) const { return lastUpdate_ == ctx.updateId; }
    void markActive(const GraphContext& ctx) { lastUpdate_ = ctx.updateId; }
    int32_t initCount_ = 0;

private:
    uint32_t lastUpdate_ = 0xffffffffu;
    friend class PoseNode;
};

class ValueNode : public Node {
public:
    virtual bool getBool(GraphContext&) { return false; }
    virtual float getFloat(GraphContext&) { return 0.0f; }
    virtual uint32_t getId(GraphContext&) { return 0; }
    // Bone masks: one weight per bone of the skeleton (valid for this update), or null.
    virtual const float* getMask(GraphContext&) { return nullptr; }
};

class PoseNode : public Node {
public:
    void initialize(GraphContext& ctx, const SyncTrackTime& initialTime);
    virtual const SyncTrack& syncTrack() const;
    virtual PoseResult update(GraphContext& ctx, const SyncTrackTimeRange* range = nullptr) = 0;

    float duration() const { return duration_; }
    float currentTime() const { return current_; }
    float previousTime() const { return previous_; }

protected:
    void initializeInternal(GraphContext& ctx) final { initializePose(ctx, SyncTrackTime{}); }
    virtual void initializePose(GraphContext& ctx, const SyncTrackTime& initialTime);
    float duration_ = 0.0f, current_ = 0.0f, previous_ = 0.0f;
};

class GraphInstance {
public:
    GraphInstance() = default;
    GraphInstance(const GraphInstance&) = delete;
    GraphInstance& operator=(const GraphInstance&) = delete;
    ~GraphInstance();

    bool init(std::shared_ptr<const GraphDefinition> definition, const std::vector<Clip>& clips, const Skeleton* skeleton);
    bool init(const AnimData& data, uint32_t graphIndex, const std::vector<Clip>& clips, const Skeleton* skeleton)
    {
        return init(GraphDefinition::fromPak(data, graphIndex), clips, skeleton);
    }
    const GraphDefinition& definition() const { return *def_; }

    // Control parameters (by FNV-1a name hash; anim_format.h hashId). Index -1 = unknown.
    int32_t parameterIndex(uint32_t nameHash) const;
    int32_t parameterIndex(const char* name) const { return parameterIndex(fmt::hashId(name)); }
    void setBool(int32_t index, bool v);
    void setFloat(int32_t index, float v);
    void setId(int32_t index, uint32_t v);
    float getFloat(int32_t index);
    bool getBool(int32_t index);
    uint32_t getId(int32_t index);
    uint32_t parameterCount() const { return uint32_t(def_->parameters.size()); }
    const std::string& parameterName(uint32_t index) const { return def_->parameters[index]; }
    fmt::NodeType parameterType(uint32_t index) const { return def_->nodes[index].type; }  // ParamBool/Float/Id

    // Debugging: the node index a node plays now (a state machine: its active state), or -1.
    int32_t activeNode(int32_t index) const;
    // Whether a state machine the last evaluate updated is in a state of that name (by hash; a pak
    // cooked before states carried their names: compiled graphs only).
    bool inState(uint32_t nameHash) const;
    // The name of the active state of the n-th state machine the last evaluate updated (graph
    // order: outer machines first), empty when there is none or the graph has no names (a pak's).
    std::string stateName(uint32_t n) const;
    // Debugging: every node as the last evaluate left it.
    struct NodeDebug {
        uint8_t flags = 0;       // 1: initialized, 2: updated by the last evaluate
        int32_t active = -1;     // a state machine: its active state's node
        float time = 0.0f;       // a pose node: how far through (0..1)
        float duration = 0.0f;   // its duration (seconds)
        float progress = -1.0f;  // a transition: how far through (0..1)
    };
    void debug(std::vector<NodeDebug>& out) const;

    // The events of the last update (clips' and states', weighted).
    const std::vector<SampledEvent>& events() const { return events_; }

    // One update: node tree -> tasks; returns the root motion delta. Then pose() runs them.
    Xform evaluate(float dt);
    const Pose& pose() { return tasks_.execute(rootTask_); }

    // For nodes, while linking.
    template <typename T>
    T* node(int32_t index) const { return index >= 0 && index < int32_t(nodes_.size()) ? static_cast<T*>(nodes_[index]) : nullptr; }
    const uint32_t* pool(uint32_t offset) const { return pool_ + offset; }
    const Clip* clip(int32_t index) const;
    const Skeleton* skeleton() const { return skeleton_; }

private:
    std::shared_ptr<const GraphDefinition> def_;
    const std::vector<Clip>* clips_ = nullptr;
    const Skeleton* skeleton_ = nullptr;
    const uint32_t* pool_ = nullptr;
    std::unique_ptr<uint8_t[]> memory_;
    std::vector<Node*> nodes_;
    std::vector<uint32_t> parameterHashes_;
    PoseNode* root_ = nullptr;
    GraphContext ctx_;
    TaskSystem tasks_;
    std::vector<SampledEvent> events_;
    int8_t rootTask_ = TaskSystem::kInvalid;  // the last evaluate's result
};

}  // namespace fx::anim
