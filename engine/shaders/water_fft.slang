// Ocean waves (render/water_ocean.cpp): Tessendorf's FFT ocean, per cascade (a square of
// `size` metres repeating over the sea, N x N waves), every frame:
//   cs_spectrum  the spectrum at this time from its initial amplitudes h0(k) (made on the CPU
//                from a JONSWAP spectrum and a seed, so the CPU can evaluate the same waves):
//                height, horizontal displacement and their slopes, packed two real fields per
//                complex number (their inverse transforms are real: the pair comes back as the
//                real and imaginary parts)
//   cs_ifft_rows / cs_ifft_columns   the inverse FFT, a row (column) per group, radix-2 Stockham
//                in shared memory
//   cs_assemble  displacement (x, y, z) and the Jacobian (where the surface folds: whitecaps,
//                foam that lingers and fades), and the slopes for the normals
// Bindings follow the compute layout: space0 read-only, space1 read-write, space2 uniforms.

static const float kPi = 3.14159265;

struct FftUniforms
{
    float4 cascade;  // x: size (m), y: N, z: time (s), w: choppiness
    float4 foam;     // x: decay since the last update, y: threshold, z: gain, w: log2(N)
};

[[vk::binding(0, 0)]] ByteAddressBuffer h0 : register(t0, space0);  // per texel: h0(k), conj(h0(-k)) (float4), then omega (float)
[[vk::binding(0, 1)]] [format("rgba32f")] RWTexture2D<float4> fieldA : register(u0, space1);  // (dx + i dz), (h + i hx)
[[vk::binding(1, 1)]] [format("rgba32f")] RWTexture2D<float4> fieldB : register(u1, space1);  // (hz + i dxdx), (dzdz + i dxdz)
[[vk::binding(2, 1)]] [format("rgba16f")] RWTexture2D<float4> displacement : register(u2, space1);  // xyz, w: foam
[[vk::binding(3, 1)]] [format("rgba16f")] RWTexture2D<float4> slopes : register(u3, space1);  // x: dh/dx, y: dh/dz, z: Jacobian, w: foam
[[vk::binding(0, 2)]] ConstantBuffer<FftUniforms> u : register(b0, space2);

float2 cmul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }
float2 cexpi(float a) { return float2(cos(a), sin(a)); }
// i * c
float2 ci(float2 c) { return float2(-c.y, c.x); }

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_spectrum(uint3 tid : SV_DispatchThreadID)
{
    const uint n = uint(u.cascade.y);
    if (tid.x >= n || tid.y >= n)
        return;
    const uint i = tid.y * n + tid.x;
    const float4 h = h0.Load<float4>(i * 16);
    const float omega = asfloat(h0.Load(n * n * 16 + i * 4));
    const float2 k = (float2(tid.xy) - float(n / 2)) * (2.0 * kPi / u.cascade.x);
    const float kl = length(k);
    const float2 e = cexpi(omega * u.cascade.z);
    // h(k, t) = h0(k) e^(i w t) + conj(h0(-k)) e^(-i w t)
    const float2 ht = cmul(h.xy, e) + cmul(h.zw, float2(e.x, -e.y));
    const float2 kn = kl > 1e-6 ? k / kl : 0.0;
    // Displacements D = -i k/|k| h (Tessendorf: the surface moves toward crests), slopes i k h.
    const float2 dx = ci(ht) * -kn.x, dz = ci(ht) * -kn.y;
    const float2 hx = ci(ht) * k.x, hz = ci(ht) * k.y;
    const float2 dxdx = ht * (kl > 1e-6 ? k.x * k.x / kl : 0.0);
    const float2 dzdz = ht * (kl > 1e-6 ? k.y * k.y / kl : 0.0);
    const float2 dxdz = ht * (kl > 1e-6 ? k.x * k.y / kl : 0.0);
    // Pairs packed as a + i b (each inverse transform is real).
    fieldA[tid.xy] = float4(dx + ci(dz), ht + ci(hx));
    fieldB[tid.xy] = float4(hz + ci(dxdx), dzdz + ci(dxdz));
}

// One inverse FFT along a line of N complex values (4 fields at once), in shared memory.
static const uint kMaxN = 256;  // (the largest FFT: WaterSystem::setFftSize clamps to it)
groupshared float4 gA[2][kMaxN];  // field A's two complex numbers
groupshared float4 gB[2][kMaxN];

void ifftLine(uint t, uint n, uint logN)
{
    uint src = 0;
    for (uint stage = 0; stage < logN; ++stage)
    {
        // Stockham autosort, radix 2: each thread one butterfly per half of the line.
        for (uint b = t; b < n / 2; b += n / 2)
        {
            const uint half = 1u << stage;
            const uint j = b % half, group = b / half;
            const uint i0 = group * half + j, i1 = i0 + n / 2;
            const float2 w = cexpi(2.0 * kPi * float(j) / float(2 * half));  // (+: inverse)
            const uint o0 = group * 2 * half + j, o1 = o0 + half;
            const float4 a0 = gA[src][i0], a1 = gA[src][i1];
            const float4 b0 = gB[src][i0], b1 = gB[src][i1];
            const float4 ta = float4(cmul(w, a1.xy), cmul(w, a1.zw));
            const float4 tb = float4(cmul(w, b1.xy), cmul(w, b1.zw));
            gA[1 - src][o0] = a0 + ta;
            gA[1 - src][o1] = a0 - ta;
            gB[1 - src][o0] = b0 + tb;
            gB[1 - src][o1] = b0 - tb;
        }
        GroupMemoryBarrierWithGroupSync();
        src = 1 - src;
    }
    // (the result is in gA[src], gB[src]; the caller reads src = logN & 1)
}

[shader("compute")]
[numthreads(256, 1, 1)]
void cs_ifft_rows(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint n = uint(u.cascade.y), logN = uint(u.foam.w);
    const uint row = group.x;
    for (uint x = thread.x; x < n; x += 256)
    {
        gA[0][x] = fieldA[uint2(x, row)];
        gB[0][x] = fieldB[uint2(x, row)];
    }
    GroupMemoryBarrierWithGroupSync();
    ifftLine(thread.x, n, logN);
    const uint src = logN & 1u;
    for (uint x = thread.x; x < n; x += 256)
    {
        fieldA[uint2(x, row)] = gA[src][x];
        fieldB[uint2(x, row)] = gB[src][x];
    }
}

[shader("compute")]
[numthreads(256, 1, 1)]
void cs_ifft_columns(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint n = uint(u.cascade.y), logN = uint(u.foam.w);
    const uint column = group.x;
    for (uint y = thread.x; y < n; y += 256)
    {
        gA[0][y] = fieldA[uint2(column, y)];
        gB[0][y] = fieldB[uint2(column, y)];
    }
    GroupMemoryBarrierWithGroupSync();
    ifftLine(thread.x, n, logN);
    const uint src = logN & 1u;
    for (uint y = thread.x; y < n; y += 256)
    {
        fieldA[uint2(column, y)] = gA[src][y];
        fieldB[uint2(column, y)] = gB[src][y];
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_assemble(uint3 tid : SV_DispatchThreadID)
{
    const uint n = uint(u.cascade.y);
    if (tid.x >= n || tid.y >= n)
        return;
    // The spectrum was centred (k = 0 at n / 2): every other texel's sign flips.
    const float sign = ((tid.x + tid.y) & 1u) != 0 ? -1.0 : 1.0;
    const float4 a = fieldA[tid.xy] * sign, b = fieldB[tid.xy] * sign;
    const float chop = u.cascade.w;
    const float dx = a.x * chop, dz = a.y * chop, h = a.z;
    const float hx = a.w, hz = b.x;
    const float dxdx = b.y * chop, dzdz = b.z * chop, dxdz = b.w * chop;
    // Jacobian of the horizontal displacement: < 1 where the surface is squeezed, < 0 folded.
    const float jacobian = (1.0 + dxdx) * (1.0 + dzdz) - dxdz * dxdz;
    // Foam: made where the surface folds, fading over time (persistence).
    const float made = saturate((u.foam.y - jacobian) * u.foam.z);
    const float foam = max(displacement[tid.xy].w * u.foam.x, made);
    displacement[tid.xy] = float4(dx, h, dz, foam);
    // Slopes of the displaced surface, per unit of the still surface's x, z.
    slopes[tid.xy] = float4(hx / max(1.0 + dxdx, 0.1), hz / max(1.0 + dzdz, 0.1), jacobian, foam);
}

