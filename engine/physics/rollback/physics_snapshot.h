#pragma once

// A Box3D world's whole state, kept in memory for rollback (physics_snapshot.c, built with Box3D):
// bodies, shapes, contacts and their manifolds, joints, sensors, islands, the broad phase and the
// constraint graph. Geometry is kept by pointer (the engine's meshes and height fields must outlive
// the snapshot; hulls are held), user data is kept. Restoring puts the world back exactly as it
// was: the same ids, the same solver order, the same results after.

#include "box3d/id.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fxPhysicsSnapshot fxPhysicsSnapshot;

fxPhysicsSnapshot* fxCreatePhysicsSnapshot(void);
// The world now (the snapshot's last image replaced; its buffer reused): the image's bytes.
int fxCapturePhysics(b3WorldId world, fxPhysicsSnapshot* snapshot);
// The world as captured. False: the image doesn't fit this build (it then must not be stepped).
bool fxRestorePhysics(b3WorldId world, const fxPhysicsSnapshot* snapshot);
// Its hulls let go of (the world still valid) and its memory freed.
void fxDestroyPhysicsSnapshot(b3WorldId world, fxPhysicsSnapshot* snapshot);

#ifdef __cplusplus
}
#endif
