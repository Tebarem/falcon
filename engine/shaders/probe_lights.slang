// The point and spot lights on what a probe ray hit (render/probe_lights.h builds the grid): the
// cell round the hit lists up to 32 lights; each one's light on the surface is summed unshadowed,
// one of them is picked in proportion to it and a shadow ray sent toward it, and the sum comes
// back if that one is seen (weighted by its share, so on average each light counts as much as it
// lights the surface where it is seen). One shadow ray a hit, however many lights: the noise
// averages out over a probe's rays and updates.

module probe_lights;

import lighting;
import ray_trace;
import ray_surface;

static const uint3 kCells = uint3(32, 16, 32);

// The local lights' light on the surface (irradiance / pi, as the sun's in rayHitLight); pick in
// [0, 1).
public float3 probeLocalLight<F : IRtFilter>(ByteAddressBuffer grid, ByteAddressBuffer rt, RaySurface s, float pick, uint lane,
                                             F filter)
{
    const uint4 header = grid.Load4(16);  // lights, cells, lists, lights' start (words)
    if (header.x == 0)
        return 0.0;
    const int3 origin = asint(grid.Load3(0));
    const float size = asfloat(grid.Load(12));
    const int3 c = int3(floor(s.position / size)) - origin;
    if (any(c < 0) || any(c >= int3(kCells)))
        return 0.0;
    const uint cell = grid.Load((header.y + uint(c.x) + kCells.x * (uint(c.y) + kCells.y * uint(c.z))) * 4);
    const uint count = cell & 63u, first = cell >> 6;
    float total = 0.0;
    for (uint i = 0; i < count; ++i)
    {
        const LightGpu l = grid.Load<LightGpu>((header.w + grid.Load((header.z + first + i) * 4) * 16) * 4);
        const float3 d = l.positionRange.xyz - s.position;
        const float d2 = dot(d, d);
        const float3 toLight = d * rsqrt(max(d2, 1e-8));
        const float ndl = dot(s.normal, toLight);
        if (ndl <= 0.0 || d2 >= l.positionRange.w * l.positionRange.w)
            continue;
        const float3 e = l.color.rgb * (distanceAttenuation(d2, l.positionRange.w) * spotAttenuation(l, toLight) * ndl * 0.31830989);
        total += dot(e, float3(0.2126, 0.7152, 0.0722));
    }
    if (!(total > 0.0))
        return 0.0;
    // The one to test: where pick * total falls along their shares.
    float target = pick * total;
    float3 chosen = 0.0, toChosen = 0.0;
    float share = 0.0, distance = 0.0;
    for (uint i = 0; i < count; ++i)
    {
        const LightGpu l = grid.Load<LightGpu>((header.w + grid.Load((header.z + first + i) * 4) * 16) * 4);
        const float3 d = l.positionRange.xyz - s.position;
        const float d2 = dot(d, d);
        const float3 toLight = d * rsqrt(max(d2, 1e-8));
        const float ndl = dot(s.normal, toLight);
        if (ndl <= 0.0 || d2 >= l.positionRange.w * l.positionRange.w)
            continue;
        const float3 e = l.color.rgb * (distanceAttenuation(d2, l.positionRange.w) * spotAttenuation(l, toLight) * ndl * 0.31830989);
        const float y = dot(e, float3(0.2126, 0.7152, 0.0722));
        if (y <= 0.0)
            continue;
        chosen = e;
        share = y;
        toChosen = toLight;
        distance = sqrt(d2);
        target -= y;
        if (target < 0.0)
            break;
    }
    const float3 start = s.position + s.normal * 0.01;
    if (rtOccluded(rt, start, toChosen, max(distance - 0.02, 0.0), lane, filter))
        return 0.0;
    return chosen * (total / share);
}
