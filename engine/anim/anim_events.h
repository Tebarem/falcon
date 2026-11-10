#pragma once

// A model's clip markup (its clips' sync events and events: anim_core.h ClipSyncEvent / ClipEvent),
// kept beside it as <model without extension>.fevents (Assets/Hero.glb: Assets/Hero.fevents):
//   {"clips": {"Walk": {"sync":   [{"id": "LeftDown", "at": 0.0}, {"id": "RightDown", "at": 0.5}],
//                       "events": [{"id": "Step", "at": 0.02, "type": "foot", "foot": "left"},
//                                  {"id": "Swing", "at": 0.6, "duration": 0.2}]}}}
// Times are percentages through the clip [0, 1); "type": "marker" (the default) or "foot" ("foot":
// left / right); "duration" (default 0) makes an event that lasts. Clips are named as the scene's
// animation data names them.

#include "anim/anim_core.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fx::anim {

struct ClipMarkup {
    std::vector<ClipSyncEvent> sync;
    std::vector<ClipEvent> events;
};
using Markup = std::map<std::string, ClipMarkup>;  // by clip name

// False (and why) when the text is not markup.
bool parseMarkup(std::string_view text, Markup& out, std::string& error);
// A model's markup file: its path with the extension replaced (".fevents").
std::string markupPathFor(const std::string& model);

}  // namespace fx::anim
