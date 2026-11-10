// Camera rays and normal packing shared by the full-screen passes (resolve, reflections, water).
// Each pass keeps its own uniforms; these take the camera as arguments: forward, right and up
// (right.w = 1 / P00, up.w = 1 / P11), the render size in pixels and the frame's NDC jitter.

module view_common;

// The camera ray through a (jittered) pixel, as the raster saw it.
public float3 cameraRay(float2 pixel, float2 size, float2 jitter, float3 forward, float4 right, float4 up)
{
    const float2 ndc = float2(pixel.x / size.x * 2.0 - 1.0, 1.0 - pixel.y / size.y * 2.0) - jitter;
    return normalize(forward + right.xyz * (ndc.x * right.w) + up.xyz * (ndc.y * up.w));
}

// Octahedral normal packing (two signed components).
public float2 octEncode(float3 v)
{
    v /= abs(v.x) + abs(v.y) + abs(v.z);
    return v.z >= 0.0 ? v.xy : (1.0 - abs(v.yx)) * float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

public float3 octDecode(float2 e)
{
    float3 v = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0)
        v.xy = (1.0 - abs(v.yx)) * float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
    return normalize(v);
}
