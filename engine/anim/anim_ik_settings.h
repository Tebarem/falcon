#pragma once

// A graph's inverse kinematics (its "ik" section; anim_graph_compiler.h): what AnimationSystem
// solves after the graph's pose, each part on while present. Targets are the graph's entity
// parameters (by name: an Animator binds them, scripts set them), read where the entities are each
// frame. Weights are a constant or a float/bool parameter of the graph, read each frame.
//
//   "ik": { "feet":  {"reach": 0.45, "weight": "$Grounded"},
//           "look":  {"target": "$LookTarget", "bone": "head", "weight": 1},
//           "aim":   {"target": "$AimTarget", "hold": "$AimHold", "bone": "hand_r", "axis": [-0.98, 0.19, 0],
//                     "support": "", "spine": 1, "weight": "$Aim"},
//           "reach": [{"bone": "hand_l", "target": "$LeftHand", "weight": 1}] }
//
// feet: each foot onto the ground under it, as far as `reach` metres up or down (the legs found by
// name). look: `bone` (the head) and the spine below it turn toward the target. aim: `bone` (the hand
// holding a weapon) aims at `target` along `axis` (in the bone's space; zero: ahead as posed); with
// `hold` it is held there, pointing at `target` or else along the hold's forward (its -Z, as a camera
// looks); `support` keeps its grip ("": the other hand, "none": no hand); `spine` (0 to 1, 1 by default)
// is how much the spine turns toward the target: 0 leaves it as the clip has it, the arms alone
// bringing the bone to the hold and the wrist pointing it (a hold is needed then). reach: a hand reaches its
// target (its forearm and upper arm); "left hand" / "right hand" find it by name (hand_l, LeftHand).

#include "anim/anim_core.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fx::anim {

struct IkWeight {
    float value = 1.0f;
    std::string parameter;  // a float or bool parameter of the graph ("": the value)
    bool operator==(const IkWeight&) const = default;
};

struct IkSettings {
    struct Feet {
        bool on = false;
        float reach = 0.45f;
        IkWeight weight;
        bool operator==(const Feet&) const = default;
    } feet;
    struct Look {
        bool on = false;
        std::string target;  // an entity parameter
        std::string bone = "head";
        IkWeight weight;
        bool operator==(const Look&) const = default;
    } look;
    struct Aim {
        bool on = false;
        std::string target, hold;  // entity parameters ("": none)
        std::string bone = "hand_r";
        float3 axis = {};
        std::string support;  // "": the other hand, "none": none
        float spine = 1.0f;   // how much the spine turns to aim (0: the arms only, bringing the bone to the hold)
        IkWeight weight;
        bool operator==(const Aim& o) const
        {
            return on == o.on && target == o.target && hold == o.hold && bone == o.bone && axis.x == o.axis.x && axis.y == o.axis.y &&
                   axis.z == o.axis.z && support == o.support && spine == o.spine && weight == o.weight;
        }
    } aim;
    struct Reach {
        std::string bone, target;
        IkWeight weight;
        bool operator==(const Reach&) const = default;
    };
    std::vector<Reach> reaches;
    bool any() const { return feet.on || look.on || aim.on || !reaches.empty(); }
    bool operator==(const IkSettings&) const = default;
};

}  // namespace fx::anim
