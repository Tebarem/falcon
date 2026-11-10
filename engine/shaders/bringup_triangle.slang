// M0 bring-up: exercises the GPU-driven path end to end.
//   animate (compute) writes vertices + indirect draw args into storage buffers,
//   vs_main pulls vertices from a storage buffer (no vertex input layout),
//   the draw is SDL_DrawGPUPrimitivesIndirect with GPU-written args,
//   and SV_ClipDistance clips the triangle against a moving plane (VSM page-clip spike).

struct Vertex
{
    float4 position;
    float4 color;
};

struct AnimateParams
{
    float time;
    float aspect;
    float2 pad;
};

struct DrawParams
{
    float4 clipPlane;  // clip-space plane; fragments with dot(plane, pos) < 0 are clipped
};

// ---- compute (set 1: rw storage, set 2: uniforms)
// SDL_GPU creates raw views for storage buffers on D3D12, so buffers are always
// (RW)ByteAddressBuffer accessed with Load<T>/Store<T>.
[[vk::binding(0, 1)]] RWByteAddressBuffer outVertices : register(u0, space1);
[[vk::binding(1, 1)]] RWByteAddressBuffer outDrawArgs : register(u1, space1);
[[vk::binding(0, 2)]] ConstantBuffer<AnimateParams> animate : register(b0, space2);

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_animate(uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i < 3)
    {
        const float angle = animate.time * 0.6 + float(i) * (2.0 * 3.14159265 / 3.0);
        Vertex v;
        v.position = float4(sin(angle) * 0.75 / animate.aspect, cos(angle) * 0.75, 0.5, 1.0);
        const float3 colors[3] = { float3(1.0, 0.35, 0.2), float3(0.2, 1.0, 0.45), float3(0.25, 0.45, 1.0) };
        v.color = float4(colors[i], 1.0);
        outVertices.Store<Vertex>(i * sizeof(Vertex), v);
    }
    if (i == 0)
        outDrawArgs.Store<uint4>(0, uint4(3, 1, 0, 0));  // vertex count, instance count, first vertex, first instance
}

// ---- vertex (set 0: resources, set 1: uniforms)
[[vk::binding(0, 0)]] ByteAddressBuffer vertices : register(t0, space0);
[[vk::binding(0, 1)]] ConstantBuffer<DrawParams> draw : register(b0, space1);

struct VsOut
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float clip : SV_ClipDistance0;
};

[shader("vertex")]
VsOut vs_main(uint vertexId : SV_VertexID)
{
    const Vertex v = vertices.Load<Vertex>(vertexId * sizeof(Vertex));
    VsOut o;
    o.position = v.position;
    o.color = v.color;
    o.clip = dot(draw.clipPlane, v.position);
    return o;
}

[shader("fragment")]
float4 fs_main(VsOut i) : SV_Target0
{
    return i.color;
}
