// What a project's surface shader gets and gives (R4). A surface shader is a file in the project's
// Shaders/ with a surface function (and, if it wants, a light function); a material uses it with
// "shader": "Shaders/Toon.slang" and up to four "params" in its .fmat:
//
//   import falcon_surface;
//
//   // The surface, after the material's own textures and factors.
//   void surface(SurfaceIn i, inout SurfaceOut o)
//   {
//       o.albedo = lerp(o.albedo, float3(1.0, 0.3, 0.1), i.params.x);
//   }
//
//   // The light, after the engine's (the sun, lights, ambient): toon bands, unlit...
//   void light(LightIn l, inout float3 color)
//   {
//       const float bands = max(l.params.y, 1.0);
//       color = l.albedo * l.sunColor * (floor(saturate(l.sunDot) * l.shadow * bands) / bands) + l.albedo * l.ambient;
//   }
//
// On a transparent material ("alpha": "blend") the same functions run in the transparent pass, where
// the surface also has its coverage (alpha) and, when the material refracts ("refraction" in its
// .fmat), how far what is seen through it is bent (refraction: a screen offset) and blurred.
//
// A vertex function (V1) moves the surface's vertices (wind, waves, a flag):
//
//   void vertex(VertexIn i, inout VertexOut o)
//   {
//       o.offset = float3(0.0, sin(i.time * 2.0 + i.world.x) * i.params.x, 0.0);
//   }
//
// It runs once a frame per vertex of the instances drawn with the material near the camera (within
// the material's "vertexDistance"; 0: any), before anything is drawn: everything (the visibility
// raster, shadows, motion vectors, GI, picking, culling) sees the moved surface. It must be a pure
// function of what it gets. "vertexBound" in the .fmat: how far it moves vertices at most (metres;
// the instance's culling sphere grows by it).
//
// The editor compiles every surface shader of the project into its own builds of the engine's
// resolve, transparent pass and vertex pass (Cache/Shaders/resolve2.fshader, transparent.fshader,
// deform.fshader), each file in a namespace of its own.

struct SurfaceIn
{
    float3 position;   // world
    float3 normal;     // the geometry's (world, toward the camera)
    float3 view;       // toward the camera
    float2 uv;
    float4 params;     // the material's "params"
    float4 baseColor;  // the material's factor
    float time;        // seconds
    float2 screenUv;   // where it is on screen (0..1, y down)
    bool transparent;  // drawn in the transparent pass ("alpha": "blend")
};

struct SurfaceOut
{
    float3 albedo;
    float roughness;
    float metallic;
    float ao;
    float3 emissive;  // added after the light
    float3 normal;    // shading normal (world)
    // Transparent materials only (the transparent pass; the resolve draws opaque):
    float alpha;          // coverage: the material's (base colour's alpha), changeable
    float2 refraction;    // screen offset (0..1 units) of what is seen through it; from the
                          // material's "refraction" by the normal (refracting materials only)
    float refractionBlur; // mip of the frame seen through it (0 sharp; from the roughness)
};

struct VertexIn
{
    float3 position;  // the mesh's (model space)
    float3 world;     // world
    float3 normal;    // world
    float2 uv;
    float4 params;    // the material's "params"
    float time;       // seconds
    float3 origin;    // the instance's position (world)
    float random;     // 0..1, the same for an instance every frame
};

struct VertexOut
{
    float3 offset;  // world: added to the position (0: where it was)
    float3 normal;  // world (the input's unless changed)
};

struct LightIn
{
    float3 albedo;
    float3 normal;        // shading normal
    float3 view;
    float3 sunDirection;  // toward the sun
    float3 sunColor;
    float sunDot;         // dot(normal, sunDirection)
    float shadow;         // 0 in the sun's shadow .. 1 lit
    float3 ambient;       // the ambient light reaching it (GI, sky)
    float4 params;
    float time;
};
