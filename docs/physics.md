# Physics

Box3D (pinned alpha), driven by the job system (enkiTS; Box3D's task callbacks).
- **Static collision is cooked:**
  - The cooker simplifies each mesh's render geometry to 2 cm. Alpha-masked and blended
    materials are left out (foliage, chains).
  - It stores the result as Box3D's relocatable `b3MeshData` blob, BVH included, in a CPU-only
    `.fpak` section.
  - At load, each cooked instance becomes one static body that points at the blob in place:
    no build step. This takes 0.4 ms for Khronos Sponza (89 shapes) and 1.0 ms for Intel
    Sponza (399 shapes, 82k triangles).
- **Dynamic bodies** use the cooked built-in cube and sphere meshes through instance pools
  (`--max-bodies`, default 4096: half boxes, half spheres).
  - Pool slots are reserved in the GPU instance table at load and start hidden. A body only
    streams its transform row (and a tint).
  - The oldest body is recycled when a pool is full. Bodies that fall below the scene are
    removed.
- **Stepping:** fixed 60 Hz with 4 substeps. Rendering interpolates between the last two
  steps. Under fixed time (captures, tests), exactly one step per frame.
  - Results are deterministic across runs, worker counts and backends.
- **Streaming:** changed rows go to the scene buffer in one copy pass, one upload per run of
  consecutive slots, together with last frame's rows (for motion vectors).
- **Shadows of moving bodies:** each moved body sends a sphere around its old and new pose.
  - A compute pass re-renders every cached shadow page (sun levels and local-light faces)
    under it in place, with no reallocation and no budget.
  - Cached pages that aren't requested this frame are dropped instead.
- **ECS:** each dynamic body is a flecs entity (`RigidBody`, `MeshInstance`,
  `WorldTransform`); the transform is updated per physics step.
- **Scene entities' bodies** (the Inspector's Rigidbody and colliders, the `PhysicsBody` and
  `Collider` components) exist while the game runs: from the start of a game, and from Play to
  Stop in the editor (`PhysicsWorld::buildEntityBodies`).
  - A Rigidbody moves its entity: the interpolated pose goes into its `WorldTransform` (a
    child's `LocalTransform`), so its children and its rows follow as any moved entity's do.
    A kinematic one follows its entity instead (scripts, animation), pushing others.
  - A collider without a Rigidbody is static. A cooked instance with either gives up its
    cooked static collider meanwhile.
  - A Mesh Collider is the mesh's cooked collider when static. On a Rigidbody it is the mesh's
    bounds as a box: Box3D moves no triangle meshes, and its hull builder asserts on some
    meshes (see Known limits).
  - Shapes scale with the entity. The Rigidbody's mass is set as given; the collider gives
    its distribution.
  - Scripts' body API (`Physics` impulse and velocity) takes these entities too.
  - Stop destroys the bodies and puts every entity back.
  - Water lifts them like the spawned bodies: a Rigidbody's box or ball (a Mesh Collider's
    bounds) floats at the depth its mass gives it.
  - Not yet: several colliders on one entity, triggers, joints.
- **Cost** (Intel Sponza, 1600×900):
  - Pile settled: about 2.0 ms per frame with 2,239 bodies resting.
  - While 4,000 bodies are awake: about 4.5 ms per frame. A Box3D step takes 2–4.5 ms on
    6 workers (about 0.8 ms per frame on average).
  - The rest is GPU work: re-rendering the ~100 shadow pages under moving bodies each frame
    (only the moving casters, over each page's cached static layer; see Sun shadows).
- **Known limits:**
  - Box3D's hull builder (alpha) asserts on some Intel Sponza props, so only the built-in
    primitives get cooked hulls.
  - Large bodies against dense static meshes hit Box3D's 256-triangle contact buffer (it
    logs a warning).
