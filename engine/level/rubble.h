#pragma once
// Rubble: broken chunks lying in a heap, for what geometry damage knocks out of a level mesh (the
// damage brush drops a heap under each hole it cuts). A level mesh of lumpy, flat-faced rocks.

#include "level/poly_mesh.h"

#include <cstdint>
#include <string>

namespace fx::level {

struct RubbleSettings {
    uint32_t seed = 1;
    uint32_t count = 6;      // chunks
    float size = 0.15f;      // a chunk's size across, about (metres; the middle ones bigger)
    float spread = 0.4f;     // how far from the middle they lie (metres)
    std::string material;    // all their faces' ("": the default)
};

// The heap around its origin, resting on y = 0 (each chunk sunk a little into it). The same seed
// makes the same heap.
PolyMesh makeRubble(const RubbleSettings& settings);

}  // namespace fx::level
