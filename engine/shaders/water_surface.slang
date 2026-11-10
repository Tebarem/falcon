// Water surface raster (render/water_renderer.cpp), one render pass after the visibility buffer:
//   vs_copy / fs_copy   the scene's depth into the water depth target (a full-screen triangle),
//                       so the surface is depth-tested against the scene
//   vs_surface / fs_surface   every visible patch of every body in one instanced draw: a grid
//                       morphing between quadtree levels (CDLOD), displaced by the body's waves;
//                       writes the surface's depth, its normals and foam, and which body it is
// The composite (water_composite.slang) then shades the water's pixels.

import water_common;
import view_common;

struct CopyUniforms
{
    float4 invSize;  // xy: 1 / render size
};

[[vk::binding(0, 2)]] Sampler2D<float> sceneDepth : register(t0, space2) : register(s0, space2);
[[vk::binding(0, 3)]] ConstantBuffer<CopyUniforms> copy : register(b0, space3);

[shader("vertex")]
float4 vs_copy(uint vertexId : SV_VertexID) : SV_Position
{
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

[shader("fragment")]
float fs_copy(float4 position : SV_Position) : SV_Depth
{
    return sceneDepth.SampleLevel(position.xy * copy.invSize.xy, 0.0);
}

struct SurfaceUniforms
{
    float4x4 viewProj;  // this frame's raster (jittered)
    float4 camera;      // xyz: position, w: water time (s)
    float4 cascades;    // the ocean's cascade sizes (m), w: waves per cascade side
};

// The ocean's displacement per cascade (render/water_ocean.h): xyz, w foam.
[[vk::binding(0, 0)]] Sampler2D<float4> oceanDisplacement0 : register(t0, space0) : register(s0, space0);
[[vk::binding(1, 0)]] Sampler2D<float4> oceanDisplacement1 : register(t1, space0) : register(s1, space0);
[[vk::binding(2, 0)]] Sampler2D<float4> oceanDisplacement2 : register(t2, space0) : register(s2, space0);
[[vk::binding(3, 0)]] ByteAddressBuffer patches : register(t3, space0);
[[vk::binding(4, 0)]] ByteAddressBuffer bodies : register(t4, space0);
[[vk::binding(0, 1)]] ConstantBuffer<SurfaceUniforms> surface : register(b0, space1);
[[vk::binding(5, 0)]] ByteAddressBuffer riverPoints : register(t5, space0);  // the outlines: rivers' paths

// A river's path point k (x, z, height, distance along), two float2 slots each from `first`.
float4 riverPoint(uint first, uint k) { return riverPoints.Load<float4>((first + k * 2) * 8); }

// The ocean's three cascades at a still-surface point, each read at the detail the vertex
// spacing can show (finer cascades fade out with distance instead of aliasing).
float3 oceanOffset(float2 xz, float spacing)
{
    const float n = surface.cascades.w;
    float3 o = 0.0;
    o += oceanDisplacement0.SampleLevel(xz / surface.cascades.x, log2(max(spacing * n / surface.cascades.x, 1.0))).xyz;
    o += oceanDisplacement1.SampleLevel(xz / surface.cascades.y, log2(max(spacing * n / surface.cascades.y, 1.0))).xyz;
    o += oceanDisplacement2.SampleLevel(xz / surface.cascades.z, log2(max(spacing * n / surface.cascades.z, 1.0))).xyz;
    return o;
}

struct SurfaceOut
{
    float4 position : SV_Position;
    float3 world : WORLD;            // the displaced surface point
    float3 normal : NORMAL;          // the waves' normal
    float2 local : LOCAL;            // the still surface point, body-local (outline clipping)
    float2 still : STILL;            // the still surface point, world x, z (the ocean's maps)
    float crest : CREST;             // crest foam
    float2 flow : FLOW;              // rivers: the direction downstream (world x, z)
    nointerpolation uint body : BODY;
    nointerpolation uint face : FACE;
};

[shader("vertex")]
SurfaceOut vs_surface(uint vertexId : SV_VertexID, uint instance : SV_InstanceID)
{
    const WaterPatch p = loadWaterPatch(patches, instance);
    const WaterBody b = loadWaterBody(bodies, p.body);
    const uint quad = vertexId / 6, corner = vertexId % 6;
    const uint2 corners[6] = { uint2(0, 0), uint2(0, 1), uint2(1, 0), uint2(1, 0), uint2(0, 1), uint2(1, 1) };
    const float2 grid = float2(uint2(quad % kWaterPatchQuads, quad / kWaterPatchQuads) + corners[corner]);
    const float n = float(kWaterPatchQuads);
    if (p.face == 6)
    {
        // A river's stretch: up to 32 spans of its path along (from origin.x, origin.y spans),
        // its width across; the path's points (x, z, height, distance along) in the outlines.
        const uint first = uint(b.flow.w), count = uint(b.shape.y);
        const uint k = min(uint(p.origin.x) + min(uint(grid.x), uint(p.origin.y)), count - 1);
        const float4 a = riverPoint(first, k);
        const float4 prev = riverPoint(first, k > 0 ? k - 1 : k), next = riverPoint(first, min(k + 1, count - 1));
        const float2 along = normalize(next.xy - prev.xy + float2(1e-6, 0.0));
        const float2 local = a.xy + float2(-along.y, along.x) * ((grid.y / n - 0.5) * p.size);  // (size: its width)
        const float3 still = bodyToWorld(b, local) + float3(0.0, a.z, 0.0);
        const WaveSample w = gerstner(b, still.xz, surface.camera.w, 1.0);
        SurfaceOut r;
        r.world = still + w.offset;
        r.position = mul(surface.viewProj, float4(r.world, 1.0));
        r.normal = w.normal;
        r.local = local;
        r.still = still.xz;
        r.crest = saturate((1.0 - w.jacobian) * 1.5) * b.foam2.x;
        r.flow = normalize(bodyToWorld(b, along).xz - bodyToWorld(b, float2(0.0, 0.0)).xz);
        r.body = p.body;
        r.face = 6;
        return r;
    }
    if (p.face != 0)
    {
        // A tank's side (u along its edge, v down to its bottom; the top row follows the waves)
        // or its bottom (origin, size x morphStart: its corner and extent).
        const float2 uv = grid / n;
        float2 local;
        float y;
        float3 normal;
        if (p.face == 5)
        {
            local = p.origin + float2(uv.x * p.size, uv.y * p.morphStart);
            y = b.posYaw.y - b.shape.z;
            normal = float3(0.0, -1.0, 0.0);
        }
        else
        {
            const float along = p.origin.x + uv.x * p.size;
            local = p.face == 1 ? float2(-b.shape.y, along) : p.face == 2 ? float2(b.shape.y, along)
                  : p.face == 3 ? float2(along, -b.shape.w) : float2(along, b.shape.w);
            const float3 w = bodyToWorld(b, local);
            const float top = b.posYaw.y + gerstner(b, w.xz, surface.camera.w, 1.0).offset.y;
            y = lerp(top, b.posYaw.y - b.shape.z, uv.y);
            const float2 ln = p.face == 1 ? float2(-1, 0) : p.face == 2 ? float2(1, 0) : p.face == 3 ? float2(0, -1) : float2(0, 1);
            normal = normalize(bodyToWorld(b, ln) - bodyToWorld(b, float2(0.0, 0.0)));
        }
        SurfaceOut f;
        f.world = float3(bodyToWorld(b, local).x, y, bodyToWorld(b, local).z);
        f.position = mul(surface.viewProj, float4(f.world, 1.0));
        f.normal = normal;
        f.local = float2(0.0, 0.0);  // (inside its outline)
        f.still = f.world.xz;
        f.crest = 0.0;
        f.flow = float2(0.0, 0.0);
        f.body = p.body;
        f.face = p.face;
        return f;
    }

    // CDLOD morph: odd grid vertices slide onto their even neighbours' line as the camera moves
    // away, until the patch matches its parent's grid where the parent takes over.
    const float3 still = bodyToWorld(b, p.origin + grid / n * p.size);
    const float morph = saturate((distance(still, surface.camera.xyz) - p.morphStart) / max(p.morphEnd - p.morphStart, 1e-3));
    const float2 morphed = grid - frac(grid * 0.5) * 2.0 * morph;
    const float2 local = p.origin + morphed / n * p.size;
    const float3 world = bodyToWorld(b, local);

    SurfaceOut o;
    if (oceanWaves(b))
    {
        o.world = world + oceanOffset(world.xz, p.size / n * (1.0 + morph));
        o.normal = float3(0.0, 1.0, 0.0);  // (from the slope maps, per pixel)
        o.crest = 0.0;
    }
    else
    {
        const WaveSample w = gerstner(b, world.xz, surface.camera.w, 1.0);
        o.world = world + w.offset;
        o.normal = w.normal;
        o.crest = saturate((1.0 - w.jacobian) * 1.5) * b.foam2.x;
    }
    o.position = mul(surface.viewProj, float4(o.world, 1.0));
    o.local = local;
    o.still = world.xz;
    o.flow = b.flow.xy;
    o.face = 0;
    o.body = p.body;
    return o;
}

struct FragmentUniforms
{
    float4 camera;    // xyz: position, w: water time (s)
    float4 cascades;  // the ocean's cascade sizes (m)
    float4 ripples;   // xy: world x, z of the ripple square's corner, z: its size (m), w: 1 / its texels
};

// Tileable detail: rg the ripples' slope (signed, 0.5 = flat), b foam pattern, a film pattern.
[[vk::binding(0, 2)]] Sampler2D<float4> detailMap : register(t0, space2) : register(s0, space2);
// The ocean's slopes per cascade: xy dh/dx, dh/dz, z Jacobian, w foam.
[[vk::binding(1, 2)]] Sampler2D<float4> oceanSlopes0 : register(t1, space2) : register(s1, space2);
[[vk::binding(2, 2)]] Sampler2D<float4> oceanSlopes1 : register(t2, space2) : register(s2, space2);
[[vk::binding(3, 2)]] Sampler2D<float4> oceanSlopes2 : register(t3, space2) : register(s3, space2);
// Live ripples (water_ripples.slang): r the height over the square around the camera.
[[vk::binding(4, 2)]] Sampler2D<float2> rippleMap : register(t4, space2) : register(s4, space2);
[[vk::binding(5, 2)]] ByteAddressBuffer fragBodies : register(t5, space2);
[[vk::binding(6, 2)]] ByteAddressBuffer outlines : register(t6, space2);  // polygons' points
[[vk::binding(0, 3)]] ConstantBuffer<FragmentUniforms> frag : register(b0, space3);

struct SurfaceTargets
{
    float4 normals : SV_Target0;  // xy: detail normal, zw: wave normal (octahedral)
    float4 extra : SV_Target1;    // x: crest foam, y: +-(body + 1) (- seen from below), z: specular AA roughness, w: 1 on a tank's sides and bottom
};

[shader("fragment")]
SurfaceTargets fs_surface(SurfaceOut i, bool front : SV_IsFrontFace)
{
    const WaterBody b = loadWaterBody(fragBodies, i.body);
    if (i.face != 6 && !insideShape(b, i.local, outlines))
        discard;
    // Two layers of small ripples drifting at different angles and sizes.
    const float t = frag.camera.w;
    const float scale = 1.0 / b.detail.y;
    const float2 wind = normalize(b.wave[0].xy + float2(1e-4, 0.0)) * b.detail.z;
    // The flow (rivers: downstream here) moves the ripples; flow-mapped in two phases half a cycle
    // apart, each reset when its weight is zero, so a bending flow never stretches them for long.
    const float2 drift = normalize(i.flow + float2(1e-6, 0.0)) * b.caustic.w;
    float2 slope = 0.0;
    for (uint phase = 0; phase < 2; ++phase)
    {
        const float cycle = frac(t / 4.0 + float(phase) * 0.5);
        const float weight = 1.0 - abs(cycle * 2.0 - 1.0);
        const float2 shift = drift * (cycle * 4.0) + wind * t + float2(0.37, 0.71) * float(phase);
        const float2 uv1 = (i.world.xz - shift) * scale;
        const float2 uv2 = float2(-i.world.z, i.world.x) * (scale * 0.61) - float2(-shift.y, shift.x) * (scale * 0.61);
        const float4 d1 = detailMap.Sample(uv1);
        const float4 d2 = detailMap.Sample(uv2);
        slope += ((d1.xy - 0.5) + float2(d2.y - 0.5, -(d2.x - 0.5))) * weight;  // (the second layer's slope, turned back)
    }
    slope *= b.detail.x;
    // Live ripples: the slope of the simulated height, inside its square.
    if (b.ripple.x > 0.0 && frag.ripples.z > 0.0)
    {
        const float2 uv = (i.world.xz - frag.ripples.xy) / frag.ripples.z;
        if (all(uv > 0.0) && all(uv < 1.0))
        {
            const float e = frag.ripples.w, texel = frag.ripples.z * e;
            const float hx = rippleMap.SampleLevel(uv + float2(e, 0.0), 0.0).x - rippleMap.SampleLevel(uv - float2(e, 0.0), 0.0).x;
            const float hz = rippleMap.SampleLevel(uv + float2(0.0, e), 0.0).x - rippleMap.SampleLevel(uv - float2(0.0, e), 0.0).x;
            slope += float2(hx, hz) / (2.0 * texel) * b.ripple.x;
        }
    }
    float3 macro = normalize(i.normal);
    float crest = i.crest;
    if (i.face != 0)
    {
        // A tank's side or bottom: flat, no ripples.
        const float3 m = front ? macro : -macro;
        SurfaceTargets f;
        f.normals = float4(octEncode(m), octEncode(m));
        f.extra = float4(0.0, front ? float(i.body + 1) : -float(i.body + 1), 0.0, 1.0);
        return f;
    }
    if (oceanWaves(b))
    {
        // The ocean's slopes and foam, all three cascades (mips by the pixel's footprint).
        const float4 s0 = oceanSlopes0.Sample(i.still / frag.cascades.x);
        const float4 s1 = oceanSlopes1.Sample(i.still / frag.cascades.y);
        const float4 s2 = oceanSlopes2.Sample(i.still / frag.cascades.z);
        const float2 s = s0.xy + s1.xy + s2.xy;
        macro = normalize(float3(-s.x, 1.0, -s.y));
        crest = saturate(s0.w + s1.w + s2.w) * b.foam2.x;
    }
    if (!front)
        macro = -macro;
    float3 n = normalize(macro + float3(slope.x, 0.0, slope.y) * (front ? -1.0 : 1.0));
    // Geometric specular anti-aliasing (Kaplanyan 2016): the normal's change across the pixel as
    // extra roughness, so ripples smaller than a pixel blur the highlight instead of sparkling.
    const float3 dx = ddx(n), dy = ddy(n);
    const float aa = sqrt(min(2.0 * 0.25 * (dot(dx, dx) + dot(dy, dy)), 0.18));

    SurfaceTargets o;
    o.normals = float4(octEncode(n), octEncode(macro));
    // w: rivers: 2 + the downstream direction's angle (turns), for the composite's foam streaks.
    const float flowTurns = atan2(i.flow.y, i.flow.x) / (2.0 * 3.14159265);
    o.extra = float4(crest, front ? float(i.body + 1) : -float(i.body + 1), aa, i.face == 6 ? 2.0 + frac(flowTurns) : 0.0);
    return o;
}
