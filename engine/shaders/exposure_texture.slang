// FSR's exposure input: the exposure the tonemapper will use, as a 1x1 R32F texture (the auto
// exposure state lives in a buffer: exposure.slang).

struct ExposureTextureUniforms
{
    float4 params;  // x: manual exposure (> 0 replaces the measured one)
};

[[vk::binding(0, 0)]] ByteAddressBuffer state : register(t0, space0);  // [256 bins] [exposure, ...]
[[vk::binding(0, 1)]] [format("r32f")] RWTexture2D<float> exposure : register(u0, space1);
[[vk::binding(0, 2)]] ConstantBuffer<ExposureTextureUniforms> u : register(b0, space2);

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_exposure_texture(uint3 tid : SV_DispatchThreadID)
{
    const float measured = asfloat(state.Load(256 * 4));
    exposure[uint2(0, 0)] = u.params.x > 0.0 ? u.params.x : (measured > 0.0 ? measured : 1.0);
}
