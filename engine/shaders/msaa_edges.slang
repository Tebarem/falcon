// MSAA edges for the visibility-buffer renderer. SDL_GPU can't read multisampled textures, so
// shading stays at one sample per pixel (visbuffer_resolve): this pass rasterizes the frame again at N
// samples per pixel and gives every sample the shaded colour of a nearby pixel that sees the
// same surface (msaa_common.slangh). The colour target's resolve averages the samples, so
// silhouettes get their coverage-weighted edge.
//   vs_fill + fs_fill    samples no triangle covers: the sky's colour from the 3x3
//   vs_main + fs_main    the opaque visibility draws replayed
// Alpha-masked clusters: msaa_edges_masked.slang (its alpha test needs more resources).

[[vk::binding(0, 2)]] Sampler2D<float4> shaded : register(t0, space2) : register(s0, space2);
[[vk::binding(1, 2)]] Sampler2D<float> depth1x : register(t1, space2) : register(s1, space2);
[[vk::binding(2, 2)]] Texture2D<uint> visBuffer : register(t2, space2);

#include "msaa_common.slangh"

struct FillOut
{
    float4 position : SV_Position;
};

[shader("vertex")]
FillOut vs_fill(uint id : SV_VertexID)
{
    FillOut o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fs_fill(FillOut i) : SV_Target0
{
    const int2 pixel = int2(i.position.xy);
    if (!isSky(pixel))
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                const int2 q = clampPixel(pixel + int2(x, y));
                if (isSky(q))
                    return float4(colorAt(q), 1.0);
            }
    return float4(colorAt(pixel), 1.0);
}

struct VsOut
{
    float4 position : SV_Position;
    nointerpolation uint id : VISIBLE;
};

[shader("vertex")]
VsOut vs_main(uint index : SV_VertexID)
{
    const VisVertex v = visVertex(index);
    VsOut o;
    o.position = v.position;
    o.id = v.id;
    return o;
}

[shader("fragment")]
float4 fs_main(VsOut i, uint primitive : SV_PrimitiveID) : SV_Target0
{
    return float4(reconstruct(int2(i.position.xy), visibilityId(i.id, primitive), i.position.z), 1.0);
}
