// What the reflection passes share (reflections.slang's screen-space trace and reflect,
// reflection_far.slang's trace off the screen): their uniforms, the surface a pixel shows, and
// the direction its far reflection takes.

module reflection_common;

import probe_common;
import view_common;

public struct ReflectUniforms
{
    public float4 cameraPos;
    public float4 cameraRight;    // w = 1 / P00
    public float4 cameraUp;       // w = 1 / P11
    public float4 cameraForward;
    public float4 jitter;         // xy: NDC jitter of this frame's raster
    public uint4 params;          // width, height, debug mode (13: the reflections alone), frame
    public ProbeVolume probes;    // probe GI (probe_common.slang)
    public float4 sun;            // xyz: toward the sun; w: bounce intensity
    public float4 sunColor;       // rgb: its irradiance; w: sky intensity
    public float4 reflect;        // x: GI on, y: screen-space traced below this roughness, z: zNear
    public float4 ssr;            // x: thickness (times the default), y: intensity, z: steps per ray, w: trace scale (1, 2: half resolution)
    public uint4 trace;           // x: VT page-table byte offset, y: the BVH is there, z: alpha-masked holes
};

// The surface a pixel shows: world position, shading and geometric normals (from the depth and
// the resolve's normals, octahedral: xy shading, zw geometric).
public void reflectSurface(ReflectUniforms v, Sampler2D<float> depth, Sampler2D<float4> normals, float2 pixel, out float3 wpos,
                           out float3 n, out float3 geometric)
{
    const float2 uv = pixel / float2(v.params.xy);
    const float3 dir = cameraRay(pixel, float2(v.params.xy), v.jitter.xy, v.cameraForward.xyz, v.cameraRight, v.cameraUp);
    const float viewZ = v.reflect.z / max(depth.SampleLevel(uv, 0.0), 1e-12);
    wpos = v.cameraPos.xyz + dir * (viewZ / dot(dir, v.cameraForward.xyz));
    const float4 nn = normals.SampleLevel(uv, 0.0);
    n = octDecode(nn.xy);
    geometric = octDecode(nn.zw);
}

// The far reflection's direction: aimed by the surface itself, the normal map's detail only on
// glossy ones (else it changes with the jitter from one pixel to the next).
public float3 reflectFarDirection(float3 dir, float3 n, float3 geometric, float roughness)
{
    const float3 farN = normalize(lerp(geometric, n, saturate(1.0 - roughness * 2.0)));
    const float3 r = reflect(dir, farN);
    return dot(r, geometric) < 0.0 ? reflect(dir, n) : r;
}
