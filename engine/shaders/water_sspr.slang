// Screen-space planar reflections for water (render/water_renderer.cpp): a still water surface is a
// mirror, so instead of marching each reflected ray across the screen (the reflection pass:
// coarse steps and half resolution show as stair steps and rows on a mirror), every scene pixel
// above the water's plane is mirrored across it and projected: where its reflection lands on
// screen. Three passes over the screen (Cichocki, "Screen Space Planar Reflections", 2017):
//   cs_clear    every pixel: no reflection yet
//   cs_project  every scene pixel above the plane: at its mirror's pixel, the smallest height over
//               the plane wins (atomic min): what is nearest the water hides what is above it
//   cs_resolve  the pixels that won write where they are (x | y << 16) at their mirror's pixel
//               (ties: the lowest)
// The composite then reads, at each pixel of that water body (bent by its normal), which pixel of
// the frame it reflects. Pixels nothing landed on (off screen, behind the camera, the sky) fall back
// to the reflection pass.

struct SsprUniforms
{
    float4x4 viewProj;  // this frame's raster (jittered: the depth buffer's pixels)
    float4 cameraPos;   // w: zNear
    float4 cameraRight; // w = 1 / P00
    float4 cameraUp;    // w = 1 / P11
    float4 cameraForward;
    float4 jitter;      // xy: NDC jitter
    uint4 params;       // width, height
    float4 plane;       // x: the water's height (world y)
};

[[vk::binding(0, 0)]] Sampler2D<float> sceneDepth : register(t0, space0) : register(s0, space0);
[[vk::binding(0, 1)]] RWByteAddressBuffer keys : register(u0, space1);     // per pixel: the winner's height over the plane (mm)
[[vk::binding(1, 1)]] RWByteAddressBuffer sources : register(u1, space1);  // per pixel: the pixel it reflects (x | y << 16), or ~0
[[vk::binding(0, 2)]] ConstantBuffer<SsprUniforms> u : register(b0, space2);

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_clear(uint3 tid : SV_DispatchThreadID)
{
    if (any(tid.xy >= u.params.xy))
        return;
    const uint i = tid.y * u.params.x + tid.x;
    keys.Store(i * 4, 0xffffffffu);
    sources.Store(i * 4, 0xffffffffu);
}

// The mirror of the scene point at a pixel, on screen; false when there is none to show.
bool mirrorOf(uint2 pix, out uint target, out uint key)
{
    target = 0;
    key = 0;
    const float2 size = float2(u.params.xy);
    const float d = sceneDepth.SampleLevel((float2(pix) + 0.5) / size, 0.0);
    if (d <= 0.0)
        return false;  // the sky (its reflection: the sky, the fallback's)
    const float2 ndc = float2((float(pix.x) + 0.5) / size.x * 2.0 - 1.0, 1.0 - (float(pix.y) + 0.5) / size.y * 2.0) - u.jitter.xy;
    const float3 dir = u.cameraForward.xyz + u.cameraRight.xyz * (ndc.x * u.cameraRight.w) + u.cameraUp.xyz * (ndc.y * u.cameraUp.w);
    const float3 p = u.cameraPos.xyz + dir * (u.cameraPos.w / d);  // (dir: forward-normalized, z = 1)
    const float above = p.y - u.plane.x;
    if (above <= 0.0)
        return false;  // under the water: not reflected
    const float4 clip = mul(u.viewProj, float4(p.x, u.plane.x - above, p.z, 1.0));
    if (clip.w <= 1e-4)
        return false;
    const float2 m = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5) * size;
    if (any(m < 0.0) || any(m >= size))
        return false;
    target = uint(m.y) * u.params.x + uint(m.x);
    key = uint(min(above * 1000.0, 4.0e9));
    return true;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_project(uint3 tid : SV_DispatchThreadID)
{
    if (any(tid.xy >= u.params.xy))
        return;
    uint target, key;
    if (mirrorOf(tid.xy, target, key))
    {
        uint old;
        keys.InterlockedMin(target * 4, key, old);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_resolve(uint3 tid : SV_DispatchThreadID)
{
    if (any(tid.xy >= u.params.xy))
        return;
    uint target, key;
    if (mirrorOf(tid.xy, target, key) && keys.Load(target * 4) == key)
    {
        uint old;  // (ties: the lowest pixel, every run the same)
        sources.InterlockedMin(target * 4, tid.x | tid.y << 16, old);
    }
}
