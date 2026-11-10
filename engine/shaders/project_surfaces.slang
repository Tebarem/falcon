// A project's surface shaders (R4), called by the resolve for materials that name one (their id
// in the material's flags, bits 8..15). The editor generates this module for a project from its
// Shaders/ and compiles the resolve against it (Cache/Shaders/resolve2.fshader); this one, the
// engine's own build's, has none.

import falcon_surface;

void projectSurface(uint shader, SurfaceIn i, inout SurfaceOut o) {}

void projectLight(uint shader, LightIn l, inout float3 color) {}

// (V1) Vertex functions: none here, so the vertex pass (vertex_deform) leaves vertices where they are.
static const bool kProjectHasVertex = false;
void projectVertex(uint shader, VertexIn i, inout VertexOut o) {}
