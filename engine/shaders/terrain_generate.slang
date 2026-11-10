// Terrain generation (render/terrain_generator.h): one chunk of heights (float, world metres)
// and materials (the terrain's packed uint) at full detail, with a margin, shaped by the steps
// of a generator stack in turn. Two height buffers swap roles between steps (the host binds the
// current one first).
//
//   cs_clear          zeroes the flow / deposit masks
//   cs_step           noise, terrace, plateau (heights -> heightsOut), paint (materials), each
//                     through the step's masks and amount
//   cs_blur           one axis of a box blur (smooth; the host runs x, z, x, z)
//   cs_erode_*        hydraulic + thermal erosion on a coarser grid (a cell per `factor`^2
//                     samples): init, then per iteration flux, water, transport, settle; apply
//                     adds the change in height to the full-detail heights and leaves the masks.
//
// Hydraulic erosion is the virtual pipe model (Mei et al. 2007): water on each cell flows to its
// four neighbours through pipes whose flux follows the difference in water surface; its velocity
// carries sediment up to a capacity (faster, steeper flow carries more); above it the ground is
// dissolved, below it sediment settles; sediment moves with the water (semi-Lagrangian). Thermal
// erosion moves ground from a cell to lower neighbours where the slope is steeper than the talus
// angle. Every pass reads neighbours from buffers it does not write, so results never depend on
// thread order.

import noise;

struct GenUniforms
{
    uint4 dims;      // samples x, z; erosion cells x, z
    uint4 op;        // op, noise type, octaves, erosion factor (samples per cell)
    uint4 misc;      // seed, layer, blend, axis (blur: 0 x, 1 z) / blur radius (samples) in .w
    float4 grid;     // world x, z of sample 0; spacing; 1 / spacing
    float4 noise;    // scale, height, lacunarity, gain
    float4 noise2;   // warp, warp scale, offset, sharpness
    float4 shape;    // terrace step, step sharpness, strata, amount
    float4 shape2;   // plateau level, knee, curvature radius (metres), -
    float4 maskH;    // height min, max, soft; mask noise
    float4 maskS;    // slope min, max, soft; mask noise scale
    float4 maskC;    // curvature min, max; -; -
    float4 maskF;    // flow min, max; deposit min, max
    float4 erode1;   // dt, rain, capacity, solubility
    float4 erode2;   // deposition, evaporation, talus height difference per cell, thermal rate
    float4 erode3;   // cell size (metres), -, -, -
};

[[vk::binding(0, 1)]] RWByteAddressBuffer heights : register(u0, space1);
[[vk::binding(1, 1)]] RWByteAddressBuffer heightsOut : register(u1, space1);
[[vk::binding(2, 1)]] RWByteAddressBuffer materials : register(u2, space1);
[[vk::binding(3, 1)]] RWByteAddressBuffer masks : register(u3, space1);    // float2: flow, deposit
[[vk::binding(4, 1)]] RWByteAddressBuffer cellA : register(u4, space1);    // float4: ground, water, sediment, -
[[vk::binding(5, 1)]] RWByteAddressBuffer cellFlux : register(u5, space1); // float4: -x, +x, -z, +z
[[vk::binding(6, 1)]] RWByteAddressBuffer cellB : register(u6, space1);    // float4: velocity x, z; flow, deposit (accumulated)
[[vk::binding(7, 1)]] RWByteAddressBuffer cellS : register(u7, space1);    // float4: new sediment, ground before, sin(slope), new ground
[[vk::binding(0, 2)]] ConstantBuffer<GenUniforms> gu : register(b0, space2);

static const uint kOpNoise = 0, kOpTerrace = 1, kOpPlateau = 2, kOpSmooth = 3, kOpErode = 4, kOpPaint = 5;
static const uint kNoiseFbm = 0, kNoiseRidged = 1, kNoiseBillow = 2, kNoiseEroded = 3, kNoiseCells = 4, kNoiseCrags = 5, kNoiseMesas = 6;
static const uint kBlendAdd = 0, kBlendMax = 1, kBlendMin = 2, kBlendReplace = 3, kBlendMultiply = 4;
static const float kGravity = 9.81;

// ---------------------------------------------------------------- full-detail samples

uint sampleIndex(int2 s)
{
    s = clamp(s, int2(0, 0), int2(gu.dims.xy) - 1);
    return uint(s.y) * gu.dims.x + uint(s.x);
}
float heightAt(int2 s) { return asfloat(heights.Load(sampleIndex(s) * 4)); }
float2 worldOf(int2 s) { return gu.grid.xy + float2(s) * gu.grid.z; }

// A range over a quantity that runs from `first` to `last`: an end at its limit is open (no fade).
float rangeMask(float x, float lo, float hi, float soft, float first, float last)
{
    return (lo > first ? saturate((x - lo) / soft + 0.5) : 1.0) * (hi < last ? saturate((hi - x) / soft + 0.5) : 1.0);
}

float slopeDegrees(int2 s)
{
    const float2 g = float2(heightAt(s + int2(1, 0)) - heightAt(s - int2(1, 0)), heightAt(s + int2(0, 1)) - heightAt(s - int2(0, 1)));
    return degrees(atan(length(g) * 0.5 * gu.grid.w));
}

// The mean height at `r` metres around (8 directions) over that distance: valleys +, ridges -.
float curvature(int2 s, float h)
{
    const int r = max(int(round(gu.shape2.z * gu.grid.w)), 1);
    const int d = max(int(round(float(r) * 0.70710678)), 1);
    const float mean = (heightAt(s + int2(r, 0)) + heightAt(s - int2(r, 0)) + heightAt(s + int2(0, r)) + heightAt(s - int2(0, r)) +
                        heightAt(s + int2(d, d)) + heightAt(s - int2(d, d)) + heightAt(s + int2(d, -d)) + heightAt(s + int2(-d, d))) * 0.125;
    return (mean - h) / (float(r) * gu.grid.z);
}

// Where the step takes (0..1): its height, slope and curvature ranges, and its noise mask.
float stepMask(int2 s, float h)
{
    float m = rangeMask(h, gu.maskH.x, gu.maskH.y, gu.maskH.z, -1e4, 1e4);
    if (gu.maskS.x > 0.0 || gu.maskS.y < 90.0)
        m *= rangeMask(slopeDegrees(s), gu.maskS.x, gu.maskS.y, gu.maskS.z, 0.0, 90.0);
    if (gu.maskC.x > -1.0 || gu.maskC.y < 1.0)
        m *= rangeMask(curvature(s, h), gu.maskC.x, gu.maskC.y, 0.02, -1.0, 1.0);
    if (gu.maskH.w > 0.0)
    {
        const float n = fbm(worldOf(s) / gu.maskS.w, 4, 2.0, 0.5, gu.misc.x + 977u);
        m *= lerp(1.0, smoothstep(0.38, 0.62, n), gu.maskH.w);
    }
    return m;
}

// The step's noise, 0..1.
float noiseValue(float2 world)
{
    const uint seed = gu.misc.x, octaves = gu.op.z;
    float2 w = world;
    if (gu.noise2.x > 0.0)
    {
        const float2 q = w / gu.noise2.y;
        w += gu.noise2.x * (float2(fbm(q, 4, 2.0, 0.5, seed + 31u), fbm(q + float2(5.2, 1.3), 4, 2.0, 0.5, seed + 47u)) * 2.0 - 1.0);
    }
    const float2 p = w / gu.noise.x;
    const float lac = gu.noise.z, gain = gu.noise.w, sharp = gu.noise2.w;
    switch (gu.op.y)
    {
    case kNoiseRidged:
        return ridged(p, octaves, lac, gain, sharp, seed);
    case kNoiseBillow:
        return billow(p, octaves, lac, gain, seed);
    case kNoiseEroded:
        return erodedFbm(p, octaves, lac, gain, seed);
    case kNoiseCells:
    {
        const float3 v = voronoi(p, seed);
        return pow(saturate(1.0 - v.x), sharp);
    }
    case kNoiseCrags:
    {
        const float3 v = voronoi(p, seed);
        return pow(saturate(1.0 - (v.y - v.x) * 1.5), sharp);
    }
    case kNoiseMesas:
    {
        // Flat tops at the cell's own height, cliffs toward the cell's edges.
        const float3 v = voronoi(p, seed);
        const float edge = smoothstep(0.0, 1.0, saturate((v.y - v.x) * sharp));
        return (0.35 + 0.65 * v.z) * edge;
    }
    default:
        return fbm(p, octaves, lac, gain, seed);
    }
}

// Paint: moves the sample's pair of layers toward `layer` by t (the weakest of three dropped),
// as the paint brush does. Flags (holes, scatter masks) are kept.
uint paintMaterial(uint m, uint layer, float t)
{
    uint ids[3] = { m & 0xffu, (m >> 8) & 0xffu, layer };
    const float wb = ids[0] == ids[1] ? 0.0 : float((m >> 16) & 0xffu) / 255.0;
    float ws[3] = { (1.0 - wb) * (1.0 - t), wb * (1.0 - t), t };
    for (int i = 0; i < 2; ++i)
        if (ids[i] == layer)
        {
            ws[i] += ws[2];
            ws[2] = 0.0;
        }
    const int drop = ws[0] <= ws[1] && ws[0] <= ws[2] ? 0 : (ws[1] <= ws[2] ? 1 : 2);
    int a = drop == 0 ? 1 : 0, b = drop == 2 ? 1 : 2;
    if (ws[a] < ws[b])
    {
        const int k = a;
        a = b;
        b = k;
    }
    const float sum = max(ws[a] + ws[b], 1e-6);
    const uint weightB = ids[a] == ids[b] ? 0u : uint(round(ws[b] / sum * 255.0));
    return ids[a] | ids[b] << 8 | weightB << 16 | (m & 0xff000000u);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_clear(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gu.dims.xy))
        return;
    masks.Store2(sampleIndex(int2(id.xy)) * 8, asuint(float2(0.0, 0.0)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_step(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gu.dims.xy))
        return;
    const int2 s = int2(id.xy);
    const uint i = sampleIndex(s);
    const float h = heightAt(s);
    float m = stepMask(s, h) * gu.shape.w;
    if (gu.op.x == kOpPaint)
    {
        const float2 fd = asfloat(masks.Load2(i * 8));
        m *= rangeMask(fd.x, gu.maskF.x, gu.maskF.y, 0.05, 0.0, 1.0) * rangeMask(fd.y, gu.maskF.z, gu.maskF.w, 0.05, 0.0, 1.0);
        if (m > 0.0)
            materials.Store(i * 4, paintMaterial(materials.Load(i * 4), gu.misc.y, saturate(m)));
        return;
    }
    float r = h;
    if (gu.op.x == kOpNoise)
    {
        const float v = (noiseValue(worldOf(s)) - 0.5 + gu.noise2.z) * gu.noise.y;
        switch (gu.misc.z)
        {
        case kBlendMax:
            r = max(h, v);
            break;
        case kBlendMin:
            r = min(h, v);
            break;
        case kBlendReplace:
            r = v;
            break;
        case kBlendMultiply:
            r = h * (1.0 + v / max(abs(gu.noise.y), 1e-3));
            break;
        default:
            r = h + v;
            break;
        }
    }
    else if (gu.op.x == kOpTerrace)
    {
        // Steps of `step` metres; their edges move with a strata noise; sharpness narrows the
        // slope between two steps toward a cliff.
        const float stepH = gu.shape.x;
        const float strata = (fbm(worldOf(s) / (stepH * 25.0), 3, 2.0, 0.5, gu.misc.x) - 0.5) * gu.shape.z;
        const float t = h / stepH + strata;
        const float k = floor(t), f = t - k;
        const float w = max(0.5 * (1.0 - gu.shape.y), 0.01);
        r = (k + smoothstep(0.5 - w, 0.5 + w, f) - strata) * stepH;
    }
    else if (gu.op.x == kOpPlateau)
    {
        const float level = gu.shape2.x, knee = gu.shape2.y;
        r = h > level ? level + knee * (1.0 - exp(-(h - level) / knee)) : h;
    }
    heightsOut.Store(i * 4, asuint(lerp(h, r, saturate(m))));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_blur(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gu.dims.xy))
        return;
    const int2 s = int2(id.xy);
    const int2 axis = gu.misc.w & 1u ? int2(0, 1) : int2(1, 0);
    const int radius = int(gu.misc.w >> 1);
    float sum = 0.0;
    for (int k = -radius; k <= radius; ++k)
        sum += heightAt(s + axis * k);
    const float h = heightAt(s);
    const float m = saturate(stepMask(s, h) * gu.shape.w);
    heightsOut.Store(sampleIndex(s) * 4, asuint(lerp(h, sum / float(2 * radius + 1), m)));
}

// ---------------------------------------------------------------- erosion cells

uint cellIndex(int2 c)
{
    c = clamp(c, int2(0, 0), int2(gu.dims.zw) - 1);
    return uint(c.y) * gu.dims.z + uint(c.x);
}
bool inside(int2 c) { return all(c >= 0) && all(c < int2(gu.dims.zw)); }
float4 loadA(int2 c) { return asfloat(cellA.Load4(cellIndex(c) * 16)); }
float4 loadFlux(int2 c) { return asfloat(cellFlux.Load4(cellIndex(c) * 16)); }

// A neighbour's water surface; beyond the edge, the same ground, dry (water drains out).
float surfaceOr(int2 n, float ground)
{
    if (!inside(n))
        return ground;
    const float4 b = loadA(n);
    return b.x + b.y;
}
float cellDelta(int2 q)
{
    const uint i = cellIndex(q) * 16;
    return asfloat(cellA.Load(i)) - asfloat(cellS.Load(i + 4));
}
float2 cellAccumulated(int2 q) { return asfloat(cellB.Load4(cellIndex(q) * 16)).zw; }

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_erode_init(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gu.dims.zw))
        return;
    const int2 c = int2(id.xy);
    const int f = int(gu.op.w);
    float sum = 0.0;
    for (int z = 0; z < f; ++z)
        for (int x = 0; x < f; ++x)
            sum += heightAt(c * f + int2(x, z));
    const float h = sum / float(f * f);
    const uint i = cellIndex(c) * 16;
    cellA.Store4(i, asuint(float4(h, 0.0, 0.0, 0.0)));
    cellFlux.Store4(i, asuint(float4(0.0, 0.0, 0.0, 0.0)));
    cellB.Store4(i, asuint(float4(0.0, 0.0, 0.0, 0.0)));
    cellS.Store4(i, asuint(float4(0.0, h, 0.0, h)));
}

// Outflow through the four pipes, from the difference in water surface; scaled down so a cell
// never sends more water than it has. Also the ground's slope (for the capacity).
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_erode_flux(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gu.dims.zw))
        return;
    const int2 c = int2(id.xy);
    const float4 a = loadA(c);
    const float dt = gu.erode1.x, l = gu.erode3.x;
    const float surface = a.x + a.y;
    const float4 heightsAround = float4(surfaceOr(c - int2(1, 0), a.x), surfaceOr(c + int2(1, 0), a.x),
                                        surfaceOr(c - int2(0, 1), a.x), surfaceOr(c + int2(0, 1), a.x));
    float4 flux = max(0.0, loadFlux(c) + dt * l * kGravity * (surface - heightsAround));
    const float out = flux.x + flux.y + flux.z + flux.w;
    if (out > 0.0)
        flux *= min(1.0, a.y * l * l / (out * dt));
    cellFlux.Store4(cellIndex(c) * 16, asuint(flux));
    // The ground's slope, from its neighbours (inside the grid).
    const float gx = loadA(c + int2(1, 0)).x - loadA(c - int2(1, 0)).x, gz = loadA(c + int2(0, 1)).x - loadA(c - int2(0, 1)).x;
    const float g = length(float2(gx, gz)) / (2.0 * l);
    const float4 s = asfloat(cellS.Load4(cellIndex(c) * 16));
    cellS.Store4(cellIndex(c) * 16, asuint(float4(s.x, s.y, g / sqrt(1.0 + g * g), s.w)));
}

// Water in minus out; the velocity through the cell; dissolving or depositing; rain.
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_erode_water(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gu.dims.zw))
        return;
    const int2 c = int2(id.xy);
    const uint i = cellIndex(c) * 16;
    float4 a = loadA(c);
    const float4 f = loadFlux(c);
    const float dt = gu.erode1.x, l = gu.erode3.x;
    const float fromLeft = inside(c - int2(1, 0)) ? loadFlux(c - int2(1, 0)).y : 0.0;
    const float fromRight = inside(c + int2(1, 0)) ? loadFlux(c + int2(1, 0)).x : 0.0;
    const float fromUp = inside(c - int2(0, 1)) ? loadFlux(c - int2(0, 1)).w : 0.0;
    const float fromDown = inside(c + int2(0, 1)) ? loadFlux(c + int2(0, 1)).z : 0.0;
    const float inflow = fromLeft + fromRight + fromUp + fromDown, outflow = f.x + f.y + f.z + f.w;
    const float d0 = a.y;
    const float d1 = max(d0 + dt * (inflow - outflow) / (l * l), 0.0);
    // Velocity: the flux through the cell over its water's cross-section (a film of water no
    // thinner than 5 mm, and no faster than it can cross half a cell in a step).
    const float depth = max((d0 + d1) * 0.5, 0.005);
    float2 v = float2(fromLeft - f.x + f.y - fromRight, fromUp - f.z + f.w - fromDown) * 0.5 / (l * depth);
    const float maxSpeed = 0.5 * l / dt;
    if (length(v) > maxSpeed)
        v *= maxSpeed / length(v);
    const float speed = length(v);
    const float4 s = asfloat(cellS.Load4(i));
    // Capacity: faster, steeper, deeper (up to 5 cm) flow carries more; picking up and dropping
    // take time (rates per second).
    const float capacity = gu.erode1.z * max(s.z, 0.02) * speed * saturate(d1 / 0.05) * 0.1;
    float4 b = asfloat(cellB.Load4(i));
    if (capacity > a.z)
    {
        const float take = min(gu.erode1.w * dt * (capacity - a.z), 0.01 * l);
        a.x -= take;
        a.z += take;
    }
    else
    {
        const float drop = min(gu.erode2.x * dt, 1.0) * (a.z - capacity);
        a.x += drop;
        a.z -= drop;
        b.w += drop;
    }
    a.y = d1 + gu.erode1.y * dt;
    b.xy = v;
    b.z += speed * d1 * dt;
    cellA.Store4(i, asuint(a));
    cellB.Store4(i, asuint(b));
}

// Sediment moves with the water (traced back along the velocity); thermal slumping.
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_erode_transport(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gu.dims.zw))
        return;
    const int2 c = int2(id.xy);
    const uint i = cellIndex(c) * 16;
    const float4 a = loadA(c);
    const float2 v = asfloat(cellB.Load4(i)).xy;
    const float dt = gu.erode1.x, l = gu.erode3.x;
    const float2 from = float2(c) - v * dt / l;
    const float2 fi = floor(from), ff = from - fi;
    const int2 b0 = int2(fi);
    const float s00 = loadA(b0).z, s10 = loadA(b0 + int2(1, 0)).z, s01 = loadA(b0 + int2(0, 1)).z, s11 = loadA(b0 + int2(1, 1)).z;
    const float sediment = lerp(lerp(s00, s10, ff.x), lerp(s01, s11, ff.x), ff.y);
    // Thermal: ground moves between neighbours whose difference passes the talus limit
    // (pairwise and symmetric, so no ground is made or lost).
    float ground = a.x;
    if (gu.erode2.w > 0.0)
    {
        const float talus = gu.erode2.z;
        float change = 0.0;
        for (int z = -1; z <= 1; ++z)
            for (int x = -1; x <= 1; ++x)
            {
                if ((x == 0 && z == 0) || !inside(c + int2(x, z)))
                    continue;
                const float limit = talus * ((x != 0 && z != 0) ? 1.41421356 : 1.0);
                const float diff = loadA(c + int2(x, z)).x - a.x;
                change += sign(diff) * max(abs(diff) - limit, 0.0);
            }
        ground += change * gu.erode2.w * 0.0625;
    }
    const float4 s = asfloat(cellS.Load4(i));
    cellS.Store4(i, asuint(float4(sediment, s.y, s.z, ground)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_erode_settle(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gu.dims.zw))
        return;
    const uint i = cellIndex(int2(id.xy)) * 16;
    const float4 s = asfloat(cellS.Load4(i));
    float4 a = asfloat(cellA.Load4(i));
    a.x = s.w;
    a.z = max(s.x, 0.0);
    a.y *= 1.0 - saturate(gu.erode2.y * gu.erode1.x);
    cellA.Store4(i, asuint(a));
}

// The cells' change in ground, bilinear, onto the full-detail heights (through the step's
// masks); the flow and deposit masks for later paint steps.
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_erode_apply(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gu.dims.xy))
        return;
    const int2 s = int2(id.xy);
    const float2 p = (float2(s) + 0.5) / float(gu.op.w) - 0.5;
    const float2 pi = floor(p), pf = p - pi;
    const int2 c = int2(pi);
    const float d = lerp(lerp(cellDelta(c), cellDelta(c + int2(1, 0)), pf.x), lerp(cellDelta(c + int2(0, 1)), cellDelta(c + int2(1, 1)), pf.x), pf.y);
    const float2 fd = lerp(lerp(cellAccumulated(c), cellAccumulated(c + int2(1, 0)), pf.x),
                           lerp(cellAccumulated(c + int2(0, 1)), cellAccumulated(c + int2(1, 1)), pf.x), pf.y);
    const float h = heightAt(s);
    const float m = saturate(stepMask(s, h) * gu.shape.w);
    const uint i = sampleIndex(s);
    heightsOut.Store(i * 4, asuint(h + d * m));
    // Flow: water speed x depth over time, on a log scale (sheet flow ~0.1, gullies 0.5+, rivers
    // ~1); deposit: metres laid down.
    const float flow = saturate(log2(1.0 + fd.x * 0.25) / 6.0);
    const float deposit = saturate(fd.y / 0.5);
    const float2 old = asfloat(masks.Load2(i * 8));
    masks.Store2(i * 8, asuint(lerp(old, float2(flow, deposit), m)));
}
