// Per-frame reset of the virtual texture feedback (dedup bitmask + append list).

struct ClearUniforms
{
    uint maskWords;
    uint pad0, pad1, pad2;
};

[[vk::binding(0, 1)]] RWByteAddressBuffer feedbackList : register(u0, space1);  // [count, entries...]
[[vk::binding(1, 1)]] RWByteAddressBuffer feedbackMask : register(u1, space1);  // 1 bit per page entry
[[vk::binding(0, 2)]] ConstantBuffer<ClearUniforms> clear : register(b0, space2);

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_vt_clear(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x < clear.maskWords)
        feedbackMask.Store(tid.x * 4, 0);
    if (tid.x == 0)
        feedbackList.Store(0, 0);
}
