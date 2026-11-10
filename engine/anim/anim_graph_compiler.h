#pragma once

// Animation graphs: JSON -> the flat GraphNode table of anim_format.h. Used by the cooker
// (tools/cook/build_graph.cpp, against the glTF it imports) and by the engine (project .fgraph
// files compiled at load and by the editor's Animator on every edit, against the loaded clips).
//
// A graph is one JSON object. "$Name" refers to a control parameter, "@id" to an entry of the
// graph's `nodes` map, a bare number/bool is a constant; any node may also be written inline
// where it is used. `editor` members (node positions, comments, the view) are the Animator's
// and are ignored here. Example:
//
//   { "version": 2, "name": "Fox", "skeleton": 0,
//     "parameters": { "Speed": "float", "Alert": {"type": "bool", "default": false} },
//     "root": "@loco",
//     "nodes": {
//       "loco": { "type": "stateMachine", "default": "Idle", "states": {
//         "Idle": { "pose": "@survey",
//                   "transitions": [ {"to": "Move", "when": "@moving", "duration": 0.3} ] },
//         "Move": { "pose": {"type": "blend1d", "input": "$Speed",
//                            "sources": [ {"value": 1, "pose": {"type": "clip", "clip": "Walk"}},
//                                         {"value": 4, "pose": {"type": "clip", "clip": "Run"}} ]},
//                   "transitions": [ {"to": "Idle", "when": {"type": "not", "input": "@moving"},
//                                     "duration": 0.4} ] } } },
//       "survey": { "type": "clip", "clip": "Survey" },
//       "moving": { "type": "compare", "input": "$Speed", "op": ">", "value": 0.1 } } }
//
// Node types: clip, blend1d, speedScale, selector, reference, stateMachine (states, transitions);
// and/or/not, compare, range, math, remap, clamp, idCompare, id, stateCompleted, time; event
// {ids, all, inactive} (this update's events: clips' markers and footsteps, states' events) and
// eventPercent {id, inactive}. A state's "events": {"entry", "exit", "fullyIn"} are ids it emits.
// blend2d {x, y, sources: [{value: [x, y], pose}]} (at least 3 points, not on one line);
// layers {base, layers: [{pose, weight, mask, mode: overlay|additive, sync, ignoreEvents,
// rootMotion}]}; boneMask {mask: "<name of the graph's boneMasks>"} or its own {default, bones:
// {name: weight}, propagate}; boneMaskBlend {a, b, weight}. The graph's "boneMasks": {name:
// {default, bones, propagate}}.
// A value node may feed several inputs (it is compiled once); a pose node has one parent, and a
// node using its state (stateCompleted, time) belongs to one transition's state.
// Compilation mirrors Esoterica's: parameters are nodes [0, N), children precede parents.
//
// Parameters of type "entity" ({"type": "entity"}) are references to entities: not values (no
// node reads them), an Animator binds them and scripts set them; the graph's "ik" section
// (anim_ik_settings.h) takes its targets from them.
//
// "skeleton" is an index, a skeleton's name, or a model's source ("Models/Fox.glb": its first
// skeleton; a project's models name theirs "<source>#<skin>").
//
// Every runtime node keeps the id it was compiled from (`nodeIds`): "$Param", a `nodes` key, or
// for inline nodes the path from the nearest one ("loco:Move/pose/sources/1/pose"; a state is
// "<machine>:<state>", a transition its `id` or "<machine>:<from>-><to>"). Errors are reported per
// node with that id and compilation carries on, so one pass finds them all.

#include "anim/anim_format.h"
#include "anim/anim_ik_settings.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fx {
struct Json;
}

namespace fx::anim {

class AnimData;

struct CompileMessage {
    std::string node;  // the node's id ("" = the graph)
    std::string text;
};

struct CompiledGraph {
    std::string name;
    uint32_t skeleton = 0;
    std::vector<fmt::GraphNode> nodes;
    std::vector<uint32_t> pool;
    std::vector<std::string> parameterNames;
    std::vector<std::string> entityParameters;  // "entity" parameters (not nodes)
    IkSettings ik;
    int16_t root = -1;
    std::vector<std::string> nodeIds;  // per node
    std::vector<CompileMessage> errors;
    bool ok() const { return errors.empty() && root >= 0; }
};

// The skeletons and clips a graph is compiled against.
class CompileSource {
public:
    virtual ~CompileSource() = default;
    virtual uint32_t skeletonCount() const = 0;
    virtual std::string skeletonName(uint32_t skeleton) const = 0;
    virtual int32_t clip(const std::string& name, uint32_t skeleton) const = 0;  // index, or -1
    // A skeleton's bones: names and parents (parents precede children), for bone masks.
    virtual void bones(uint32_t skeleton, std::vector<std::string>& names, std::vector<int32_t>& parents) const = 0;
};

// The loaded Animation section's skeletons and clips (clip indices are the section's).
class AnimDataSource : public CompileSource {
public:
    explicit AnimDataSource(const AnimData& data) : data_(data) {}
    uint32_t skeletonCount() const override;
    std::string skeletonName(uint32_t skeleton) const override;
    int32_t clip(const std::string& name, uint32_t skeleton) const override;
    void bones(uint32_t skeleton, std::vector<std::string>& names, std::vector<int32_t>& parents) const override;

private:
    const AnimData& data_;
};

// An "ik" section (anim_ik_settings.h) into `out`: targets must be among `entities`, weight
// parameters satisfy `isValue` (a float or bool parameter; null: none allowed). Errors are reported
// as the node "ik" (`errors`); false with any.
bool parseIk(const Json& ik, const std::vector<std::string>& entities, const std::function<bool(const std::string&)>& isValue,
             IkSettings& out, std::vector<CompileMessage>& errors);

// False when the graph has errors (out.errors lists them).
bool compileGraph(const Json& graph, const CompileSource& source, CompiledGraph& out);
bool compileGraph(std::string_view text, const CompileSource& source, CompiledGraph& out);  // parse errors too

}  // namespace fx::anim
