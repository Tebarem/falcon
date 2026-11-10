// Editor scene view (M9), drawn over the scene in the viewport (alpha blended):
//   vs_fullscreen + ps_overlay  infinite ground grid (y = 0, depth-tested against the scene,
//                               10x levels that fade with height), the selection outline
//                               (edges of the selected instance in the visibility buffer) and
//                               the terrain brush (a ring and its falloff on the surface)
//   vs_color + ps_color         CPU-built screen-space triangles: transform gizmo, orientation
//                               gizmo, hover highlights

struct OverlayUniforms
{
    float4 position;  // camera position, w: zNear
    float4 right;     // xyz, w: 1 / p00
    float4 up;        // xyz, w: 1 / p11
    float4 forward;   // xyz, w: grid opacity (0 = off)
    float4 outline;   // rgb, a: opacity (0 = no selection)
    uint4 view;       // viewport x, y, width, height (pixels)
    uint4 select;     // x: selected instance slot, yz: render size (the visibility buffer's)
    float4 brush;     // terrain brush: xyz center, w radius (0 = none)
    float4 curve[3];  // its falloff at distance / radius = 0, 1/8 .. 1 (curve[2].x); curve[2].y: 1 = lowering;
                      // curve[2].z: 1 = a ball (the level paint brush: distance in 3D), else across the ground
};

[[vk::binding(0, 2)]] Sampler2D<float> sceneDepth : register(t0, space2) : register(s0, space2);
[[vk::binding(1, 2)]] Texture2D<uint> visBuffer : register(t1, space2);
[[vk::binding(2, 2)]] ByteAddressBuffer visible : register(t2, space2);
[[vk::binding(0, 3)]] ConstantBuffer<OverlayUniforms> u : register(b0, space3);

struct FullscreenOut
{
    float4 position : SV_Position;
};

[shader("vertex")]
FullscreenOut vs_fullscreen(uint id : SV_VertexID)
{
    FullscreenOut o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// The instance in the visibility buffer under viewport pixel p (render-sized while upscaling).
uint instanceAt(int2 p)
{
    if (any(p < 0) || any(p >= int2(u.view.zw)))
        return 0xffffffffu;
    const uint2 r = min(uint2((float2(p) + 0.5) * float2(u.select.yz) / float2(u.view.zw)), u.select.yz - 1);
    const uint packed = visBuffer[r];
    return packed == 0 ? 0xffffffffu : visible.Load(((packed - 1) >> 7) * 8) & 0x7fffffffu;
}

// Where the camera ray through a pixel (viewport coordinates, pixel centers at .5) meets the
// y = 0 plane; t: view depth of the hit (the ray has a unit forward component), negative when
// the plane is behind the camera.
float3 gridHit(float2 pixel, out float t)
{
    const float2 ndc = pixel / float2(u.view.zw) * float2(2.0, -2.0) + float2(-1.0, 1.0);
    const float3 dir = u.forward.xyz + u.right.xyz * (ndc.x * u.right.w) + u.up.xyz * (ndc.y * u.up.w);
    t = -u.position.y / dir.y;
    return u.position.xyz + dir * t;
}

// Anti-aliased grid lines of one cell size: coverage of the nearest line, ~1 px wide. `width`:
// how far p moves per pixel on each axis (what fwidth would give).
float gridLines(float2 p, float2 width, float cell)
{
    const float2 c = p / cell;
    const float2 d = abs(frac(c - 0.5) - 0.5) / max(width / cell, 1e-5);
    return 1.0 - saturate(min(d.x, d.y));
}

[shader("fragment")]
float4 ps_overlay(FullscreenOut i) : SV_Target
{
    const int2 pixel = int2(i.position.xy) - int2(u.view.xy);
    float4 color = 0.0;

    // Grid on y = 0, behind the scene's depth (reverse-Z, infinite far: view z = zNear / depth).
    // Line widths come from the plane hits of the neighbouring pixels, not from fwidth: the
    // compiler may move derivatives into the depth test's branch, where a 2x2 quad that an
    // object half covers has undefined ones, and that drew dotted lines along every object's
    // outline against the floor.
    if (u.forward.w > 0.0)
    {
        const float2 center = float2(pixel) + 0.5;
        float t, tx, ty;
        const float3 hit = gridHit(center, t);
        const float2 width = abs(gridHit(center + float2(1, 0), tx).xz - hit.xz) + abs(gridHit(center + float2(0, 1), ty).xz - hit.xz);
        const float depth = sceneDepth.SampleLevel(center / float2(u.view.zw), 0.0);
        const float sceneZ = depth > 0.0 ? u.position.w / depth : 1e30;
        // Width checks: neighbours across the horizon give no usable footprint.
        if (t > 0.0 && tx > 0.0 && ty > 0.0 && t < sceneZ * 1.0005)
        {
            // Two decades of cells around the camera height, the finer one fading out.
            const float lod = log10(max(abs(u.position.y), 1.0) * 0.6);
            const float cell = pow(10.0, floor(lod));
            const float fine = gridLines(hit.xz, width, cell) * (1.0 - frac(lod));
            const float coarse = gridLines(hit.xz, width, cell * 10.0);
            const float distanceFade = saturate(1.0 - t / (max(abs(u.position.y), 1.0) * 80.0));
            float a = max(fine * 0.35, coarse * 0.6) * distanceFade;
            float3 c = float3(0.62, 0.62, 0.62);
            // World axes through the origin: X red (z = 0), Z blue (x = 0).
            const float2 axisW = max(width, 1e-5) * 1.5;
            const float onX = 1.0 - saturate(abs(hit.z) / axisW.y);
            const float onZ = 1.0 - saturate(abs(hit.x) / axisW.x);
            if (onX > 0.0) { c = lerp(c, float3(0.86, 0.26, 0.24), onX); a = max(a, onX * 0.8 * distanceFade); }
            if (onZ > 0.0) { c = lerp(c, float3(0.26, 0.48, 0.92), onZ); a = max(a, onZ * 0.8 * distanceFade); }
            color = float4(c, a * u.forward.w);
        }
    }

    // The terrain brush (across the ground) or the level paint brush (a ball round the point under
    // the mouse: a circle on a wall too): where the scene's surface lies within its radius, the
    // falloff as a tint and a ring at the edge, about 1.5 px wide wherever it falls.
    if (u.brush.w > 0.0)
    {
        const float2 center = float2(pixel) + 0.5;
        const float depth = sceneDepth.SampleLevel(center / float2(u.view.zw), 0.0);
        if (depth > 0.0)
        {
            const float sceneZ = u.position.w / depth;
            const float2 ndc = center / float2(u.view.zw) * float2(2.0, -2.0) + float2(-1.0, 1.0);
            const float3 dir = u.forward.xyz + u.right.xyz * (ndc.x * u.right.w) + u.up.xyz * (ndc.y * u.up.w);
            const float3 p = u.position.xyz + dir * sceneZ;
            const float d = (u.curve[2].z > 0.5 ? length(p - u.brush.xyz) : length(p.xz - u.brush.xz)) / u.brush.w;
            const float pixelWorld = sceneZ * 2.0 * u.up.w / float(u.view.w);
            const float ring = 1.0 - saturate(abs(d - 1.0) / max(1.5 * pixelWorld / u.brush.w, 1e-5));
            float fill = 0.0;
            if (d < 1.0)
            {
                const float f = d * 8.0;
                const uint i = min(uint(f), 7u);
                const float a = i < 4 ? u.curve[0][i] : u.curve[1][i - 4];
                const float b = i + 1 < 4 ? u.curve[0][i + 1] : (i + 1 < 8 ? u.curve[1][i + 1 - 4] : u.curve[2].x);
                fill = lerp(a, b, f - float(i));
            }
            const float3 tint = u.curve[2].y > 0.5 ? float3(0.95, 0.45, 0.25) : float3(0.35, 0.75, 1.0);
            const float a = max(ring * 0.9, fill * 0.22);
            if (a > 0.0)
                color = float4(lerp(color.rgb, ring > fill * 0.25 ? float3(1.0, 1.0, 1.0) : tint, a), max(color.a, a));
        }
    }

    // Selection outline: pixels next to (but outside) the selected instance's visible pixels.
    if (u.outline.a > 0.0)
    {
        const uint selected = u.select.x;
        const bool inside = instanceAt(pixel) == selected;
        if (!inside)
        {
            // A 2-pixel ring: the 4 neighbours at distance 1 and 2, and the diagonals.
            const int2 offsets[12] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1), int2(2, 0), int2(-2, 0),
                                       int2(0, 2), int2(0, -2), int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1) };
            float edge = 0.0;
            [unroll]
            for (int k = 0; k < 12; ++k)
                if (instanceAt(pixel + offsets[k]) == selected)
                    edge = 1.0;
            if (edge > 0.0)
                color = float4(u.outline.rgb, u.outline.a);
        }
    }
    return color;
}

// ---- screen-space colored triangles

struct ColorVertex
{
    float4 position;  // NDC xy, z unused, w: packed RGBA8 (asuint)
};

[[vk::binding(0, 0)]] ByteAddressBuffer vertices : register(t0, space0);

struct ColorOut
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

[shader("vertex")]
ColorOut vs_color(uint id : SV_VertexID)
{
    const float4 v = asfloat(vertices.Load4(id * 16));
    const uint c = asuint(v.w);
    ColorOut o;
    o.position = float4(v.xy, 0.0, 1.0);
    o.color = float4(c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff, c >> 24) / 255.0;
    return o;
}

[shader("fragment")]
float4 ps_color(ColorOut i) : SV_Target
{
    return i.color;
}
