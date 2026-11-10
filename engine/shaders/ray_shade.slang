// The light a ray through the BVH brings back from what it hit (the probe GI's rays, and the
// reflections and water past the screen): the surface's colour times the sun on it (a shadow ray
// toward it) and the probes' light there, times the bounce intensity, plus its glow.

module ray_shade;

import ray_trace;
import ray_surface;
import probe_common;

public float3 rayHitLight<F : IRtFilter, T : IProbeTextures>(ByteAddressBuffer rt, RaySurface s, float3 dir, float3 sunDir,
                                                             float3 sunColor, float bounce, ProbeVolume v, T probes,
                                                             uint lane, F filter)
{
    float3 light = 0.0;
    const float ndl = dot(s.normal, sunDir);
    if (ndl > 0.0 && any(sunColor > 0.0))
    {
        const float3 start = s.position + s.normal * 0.01;
        if (!rtOccluded(rt, start, sunDir, 1e4, lane, filter))
            light = sunColor * (ndl * 0.31830989);  // (1 / pi)
    }
    light += probeIrradiance(v, probes, s.position, s.normal, -dir);
    return s.albedo * light * bounce + s.emissive;
}
