#pragma once
// A cooked mesh read back as a PolyMesh (live cuts on imported art: level/mesh_cut.h): its full
// detail (the cluster DAG's level 0), a face per triangle with its UVs (Manual), welded by
// position, smoothed where its faces meet at under 40 degrees. Its materials are the scene's own,
// as "scene:N" slots (LevelMeshes draws them as material N).

#include "level/poly_mesh.h"

#include <cstdint>

namespace fx {
class ScenePak;
}

namespace fx::level {

bool meshFromPak(const ScenePak& pak, uint32_t mesh, PolyMesh& out);

}  // namespace fx::level
