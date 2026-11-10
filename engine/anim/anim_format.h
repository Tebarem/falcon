#pragma once

// The .fpak Animation section (CPU only): skeletons and compressed clips. Written by the
// cooker (tools/cook/build_anim.cpp), read in place by the runtime (anim/anim_data.h).
// Clip compression follows Esoterica's (MIT): per track a 48-bit rotation (smallest three),
// a 48-bit translation quantized to the track's range and a 16-bit uniform scale; tracks
// whose component never changes store it once in the track definition instead.
// All offsets are bytes from the start of the section; all records are 4-byte aligned.

#include <cstdint>

namespace fx::anim::fmt {

inline constexpr char kMagic[4] = {'F', 'A', 'N', 'M'};
inline constexpr uint32_t kVersion = 2;

struct Header {
    char magic[4];
    uint32_t version;
    uint32_t skeletonCount, clipCount;
    uint32_t skeletons;  // Skeleton[skeletonCount]
    uint32_t clips;      // Clip[clipCount]
    uint32_t strings;    // NUL-terminated UTF-8 names
    uint32_t graphCount;
    uint32_t graphs;     // Graph[graphCount]
    uint32_t pad[3];
};

struct Skeleton {
    uint32_t name;
    uint32_t boneCount;
    uint32_t bones;  // Bone[boneCount]; parents precede children, bone 0 is the root
    uint32_t pad;
};

// Reference (rest) pose in parent space, and the model -> bone inverse bind (3x4 rows).
struct Bone {
    uint32_t name;
    int32_t parent;  // -1 for bone 0
    float rotation[4];  // x, y, z, w
    float translation[3];
    float scale;  // uniform
    float inverseBind[12];
};
static_assert(sizeof(Bone) == 88);

struct FloatRange {
    float start, length;
};

struct Track {
    FloatRange translation[3];
    FloatRange scale;
    float constantRotation[4];
    uint32_t readOffset;  // first uint16 of this track within a frame's compressed pose
    uint8_t staticRotation, staticTranslation, staticScale, pad;
};
static_assert(sizeof(Track) == 56);

struct SyncEvent {
    uint32_t id;      // hash of the event name
    float startTime;  // percentage through the clip [0, 1)
};

struct RootTransform {
    float rotation[4];
    float translation[3];
    float scale;
};

struct Clip {
    uint32_t name;
    uint32_t skeleton;
    uint32_t frameCount;
    float duration;          // seconds; frame f is at f / (frameCount - 1) of the duration
    uint32_t tracks;         // Track[boneCount]
    uint32_t poseData;       // uint16 stream, all frames
    uint32_t poseOffsets;    // uint32[frameCount]: first uint16 of each frame's pose
    uint32_t rootMotion;     // RootTransform[frameCount], model space of the root bone
    uint32_t syncEvents;     // SyncEvent[syncEventCount]
    uint32_t syncEventCount;
    float averageLinearVelocity;   // m/s over the clip (root motion)
    float averageAngularVelocity;  // rad/s around the up (+Y) axis
    float totalDelta[8];           // RootTransform of the whole clip's root motion delta
    uint32_t flags;                // kClipAdditive
    uint32_t rootMotionCount;      // frameCount, or 1 for a stationary clip (deltas are identity)
    uint32_t pad[2];
};

inline constexpr uint32_t kClipAdditive = 1u << 0;

// ---- animation graphs (compiled from JSON: anim/anim_graph_compiler.h; run: anim/anim_graph.h)

enum class NodeType : uint16_t {
    // values
    ParamBool, ParamFloat, ParamId,
    ConstBool, ConstFloat, ConstId,
    And, Or, Not,
    FloatCompare, FloatRange, FloatMath, FloatRemap, FloatClamp,
    IdCompare,
    StateCompleted, TimeCondition,
    // poses
    Clip, Blend1D, SpeedScale, Selector, ReferencePose,
    State, StateMachine, Transition,
    // (appended: older paks keep their numbers)
    EventCondition, EventPercent,
    Blend2D, Layers,          // poses
    BoneMask, BoneMaskBlend,  // bone masks (values: per-bone weights)
    Count
};

// 48 bytes. Field meaning depends on the type (anim_graph.cpp documents each); `list` indexes
// the graph's uint32 pool (node indices, IDs or float bits).
struct GraphNode {
    NodeType type;
    uint16_t flags;
    int16_t n[4];  // node indices (-1 = none) or small integers
    float f[4];
    uint32_t id;
    uint32_t list, listCount;
    uint32_t extra;  // Transition: easing | root motion blend << 8
    uint32_t pad;
};
static_assert(sizeof(GraphNode) == 48);

// Parameters are nodes [0, parameterCount); `root` is a pose node.
struct Graph {
    uint32_t name;
    uint32_t skeleton;
    uint32_t nodeCount;
    uint32_t nodes;           // GraphNode[nodeCount]
    uint32_t pool;            // uint32[poolCount]
    uint32_t poolCount;
    uint32_t parameterNames;  // uint32 string offsets [parameterCount]
    uint16_t parameterCount;
    int16_t root;
};

inline uint32_t hashId(const char* s)  // FNV-1a, for IDs and parameter lookups
{
    uint32_t h = 2166136261u;
    for (; *s; ++s)
        h = (h ^ uint8_t(*s)) * 16777619u;
    return h;
}

}  // namespace fx::anim::fmt
