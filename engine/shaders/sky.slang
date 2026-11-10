// The sky: one definition for everything that shows it or is lit by it (the background, the
// reflections' misses, water), so they meet exactly at the horizon.

module sky;

// The background's gradient, as a display colour (debug views draw it as is).
public float3 skyGradient(float3 dir)
{
    return lerp(float3(0.55, 0.62, 0.70), float3(0.20, 0.36, 0.62), saturate(dir.y * 1.5));
}

// The background's radiance (linear HDR).
public float3 skyRadiance(float3 dir)
{
    return pow(skyGradient(dir), 2.2) * 2.5;
}

// The sky as the background draws it, and the ground below the horizon (the background's never
// darkens there: GI rays leaving the scene downward lit thin edges in blue).
public float3 openRadiance(float3 dir)
{
    const float3 sky = skyRadiance(dir);
    return lerp(float3(0.035, 0.032, 0.03), sky, saturate(dir.y * 4.0 + 1.0));
}

// Ambient light for a normal with GI off: a flat sky over a dark ground.
public float3 flatAmbient(float3 n)
{
    return lerp(float3(0.10, 0.09, 0.08), float3(0.30, 0.36, 0.45), n.y * 0.5 + 0.5);
}

// Sky radiance in a direction (linear HDR): zenith blue, bright horizon, dark ground. Scaled so
// an open hemisphere gets about a fifth of the default sun's irradiance (a clear day).
public float3 giSky(float3 dir)
{
    const float3 zenith = float3(0.11, 0.17, 0.29), horizon = float3(0.24, 0.26, 0.28), ground = float3(0.035, 0.032, 0.03);
    return dir.y >= 0.0 ? lerp(horizon, zenith, sqrt(saturate(dir.y))) : lerp(horizon, ground, saturate(-dir.y * 4.0));
}
