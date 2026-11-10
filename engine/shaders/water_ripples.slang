// Live ripples and wakes (render/water_renderer.cpp): the wave equation on a square of the world
// around the camera (N x N texels, `texel` metres each, anchored to the world's grid: as the
// camera moves, the square moves in whole texels and what it held moves with it). Each step:
//   h' = h + (h - h_prev) * damping + speed * laplacian(h), then the splats (bodies moving through
//   the surface, the camera at it, scripts) added as Gaussian bumps.
// The state is (h, h_prev) per texel, ping-ponged between two textures. The surface reads the
// slopes of h for its normals.

struct RippleUniforms
{
    int4 shift;    // xy: texels the square moved since the last step (read from there), z: splats, w: N
    float4 area;   // xy: the world x, z of texel (0, 0)'s corner, z: texel size (m), w: damping
    float4 wave;   // x: speed (c dt / dx)^2, at most 0.5; y: splat gain
};

struct Splat
{
    float4 at;  // xy: world x, z; z: radius (m); w: strength (m of height)
};

[[vk::binding(0, 0)]] ByteAddressBuffer splats : register(t0, space0);
[[vk::binding(0, 1)]] [format("rg16f")] RWTexture2D<float2> previous : register(u0, space1);
[[vk::binding(1, 1)]] [format("rg16f")] RWTexture2D<float2> next : register(u1, space1);
[[vk::binding(0, 2)]] ConstantBuffer<RippleUniforms> u : register(b0, space2);

float2 stateAt(int2 p)
{
    const int n = u.shift.w;
    const int2 q = p + u.shift.xy;
    return all(q >= 0) && all(q < n) ? previous[uint2(q)] : 0.0;  // (texels that came in: still)
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_step(uint3 tid : SV_DispatchThreadID)
{
    const int n = u.shift.w;
    const int2 p = int2(tid.xy);
    if (any(p >= n))
        return;
    const float2 s = stateAt(p);
    // The edges of the square absorb (no reflections back from where the simulation ends).
    const int edge = min(min(p.x, p.y), min(n - 1 - p.x, n - 1 - p.y));
    const float absorb = saturate(float(edge) / 8.0);
    const float lap = stateAt(p + int2(1, 0)).x + stateAt(p - int2(1, 0)).x + stateAt(p + int2(0, 1)).x + stateAt(p - int2(0, 1)).x - 4.0 * s.x;
    float h = s.x + (s.x - s.y) * u.area.w + u.wave.x * lap;
    h *= lerp(0.9, 1.0, absorb);
    // Splats: Gaussian bumps where things disturb the water.
    const float2 world = u.area.xy + (float2(p) + 0.5) * u.area.z;
    for (uint i = 0; i < uint(u.shift.z); ++i)
    {
        const float4 sp = splats.Load<float4>(i * 16);
        const float2 d = world - sp.xy;
        const float r2 = max(sp.z * sp.z, u.area.z * u.area.z);
        if (dot(d, d) < 9.0 * r2)
            h += sp.w * u.wave.y * exp(-dot(d, d) / r2);
    }
    next[uint2(p)] = float2(clamp(h, -2.0, 2.0), s.x);
}
