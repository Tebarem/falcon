// The HTML UI over the frame: the page's texture (Ember draws it with premultiplied alpha, at about
// the window's pixel size) blended onto the swapchain, which already holds the frame
// (engine/ui/ui_layer.cpp). One texel per pixel from the top-left corner, so text stays sharp when
// the texture is a pixel larger or smaller than the window (its size is CSS pixels times the scale,
// rounded up); past its edge nothing is drawn.

struct CompositeUniforms
{
    float4 texel;  // xy: 1 / the texture's size in pixels, zw: its size
};

[[vk::binding(0, 2)]] Sampler2D<float4> page : register(t0, space2) : register(s0, space2);
[[vk::binding(0, 3)]] ConstantBuffer<CompositeUniforms> u : register(b0, space3);

struct CompositeOut
{
    float4 position : SV_Position;
};

[shader("vertex")]
CompositeOut vs_main(uint id : SV_VertexID)
{
    CompositeOut o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fs_main(CompositeOut i) : SV_Target
{
    if (any(i.position.xy >= u.texel.zw))
        return float4(0.0, 0.0, 0.0, 0.0);
    return page.SampleLevel(i.position.xy * u.texel.xy, 0.0);
}
