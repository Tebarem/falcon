// Editor picking (M9): the instance under one pixel of the visibility buffer. One thread,
// dispatched after the frame's passes; the result is read back asynchronously with the stats.
// Output: instance slot (0xffffffff for sky / outside the target), then the scene cluster and
// the triangle in it (0xffffffff without one): which face of a level mesh is under the cursor.

struct PickUniforms
{
    uint4 pixel;  // x, y, width, height
};

[[vk::binding(0, 0)]] Texture2D<uint> visBuffer : register(t0, space0);
[[vk::binding(1, 0)]] ByteAddressBuffer visible : register(t1, space0);
[[vk::binding(0, 1)]] RWByteAddressBuffer result : register(u0, space1);
[[vk::binding(0, 2)]] ConstantBuffer<PickUniforms> u : register(b0, space2);

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_pick(uint3 tid : SV_DispatchThreadID)
{
    uint instance = 0xffffffffu, cluster = 0xffffffffu, triangle = 0xffffffffu;
    if (u.pixel.x < u.pixel.z && u.pixel.y < u.pixel.w)
    {
        const uint packed = visBuffer[u.pixel.xy];
        if (packed != 0)
        {
            const uint2 item = visible.Load2(((packed - 1) >> 7) * 8);  // (instance | flag, cluster)
            instance = item.x & 0x7fffffffu;
            cluster = item.y;
            triangle = (packed - 1) & 0x7fu;
        }
    }
    result.Store3(0, uint3(instance, cluster, triangle));
}
