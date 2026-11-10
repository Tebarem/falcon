#pragma once
// Baked cuts: what level meshes and imported art came to with their cut volumes, saved with the
// scene (<scene>.cuts, written when the editor saves it, shipped with it by Build Game) so that
// loading the scene doesn't cut them again. Each result is kept under a key of what it was cut
// from: the target's content, where it is, and each cut's shape, settings, place and mesh. A
// target or cut changed since (in the edits, or the art cooked again) gives another key, and is cut
// as usual; a key found is the very mesh that cut made.

#include "level/mesh_cut.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fx::level {

struct BakedCut {
    std::shared_ptr<const PolyMesh> mesh;  // the target cut
    std::vector<uint32_t> sourceFace;      // per face: the target's face it came from (~0u: a cap)
    bool clipped = false;                  // an open target: clipped, without caps
};

// A mesh's content (its saved blob), and a cut's key from its target's content key.
uint64_t meshKey(const PolyMesh& mesh);
uint64_t cutKey(uint64_t targetKey, const float world[3][4], const std::vector<Cut>& cuts);

struct CutBake {
    std::unordered_map<uint64_t, std::shared_ptr<const BakedCut>> results;

    std::vector<uint8_t> save() const;
    bool load(const uint8_t* data, size_t size);
    // A scene's file (none, or removed, when there is nothing in it).
    bool saveFile(const std::string& path) const;
    bool loadFile(const std::string& path);
};

// <scene>.cuts beside its .fpak.
std::string cutBakePathFor(const std::string& scenePath);

}  // namespace fx::level
