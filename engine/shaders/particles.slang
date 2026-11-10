// M16: particle sprites (render/particle_renderer.h). One instanced draw of every visible sprite,
// sorted back to front on the CPU, into the HDR frame after the scene's lighting and before
// TAA / FSR. Each instance is a quad (6 vertices) facing the camera, its velocity, up or flat;
// flipbooks show a frame (blended into the next) of their material's texture.
//   - Textures come through the virtual texture (vt.slang): pages the CPU asked for, falling
//     back to coarser resident ones.
//   - Lit sprites take the sun (with its virtual shadow map), the clustered local lights and the
//     sky's flat ambient at their centre, in the vertex shader.
//   - No depth target: the fragment shader reads the scene's depth (water's included), hides
//     what is behind it and softens where it meets surfaces (Depth feather).
//   - Premultiplied blending covers all three blend modes (additive: alpha 0).
//   - A second target takes each sprite's coverage (max blend): the reactive mask TAA and FSR use
//     to trust the current frame where particles are.

import scene_types;
import vt;
import lighting;
import vsm;
import shadows;
import sky;

struct ParticleUniforms
{
    float4x4 viewProj;    // the raster's (jittered)
    float4 cameraPos;     // w: near plane
    float4 cameraRight;
    float4 cameraUp;
    float4 cameraForward;
    float4 sunDirection;
    float4 sunColor;
    uint4 params;         // x width, y height, z page table offset (bytes), w frame
    float4 paramsF;       // x mip bias, y ambient scale
    LightGrid lightGrid;
    VsmParams vsm;
};

struct SpriteGpu
{
    float4 positionSize;  // xyz, w width
    float4 color;         // linear rgb (HDR), alpha
    float4 velocity;      // xyz world velocity, w height
    float4 params;        // x roll (radians), y frame (whole: index, fraction: blend), z feather (m), w stretch
    uint4 info;           // x material, y flags, z columns | rows << 8, w camera fade near | far << 16 (cm)
};
static const uint kSpriteStride = 80;

// info.y
static const uint kBlendMask = 3u;       // 0 alpha, 1 additive, 2 premultiplied
static const uint kAlignShift = 2u;      // 0 camera, 1 velocity, 2 upright, 3 flat
static const uint kFlagLit = 1u << 4;
static const uint kFlagFaceVelocity = 1u << 5;
static const uint kFlagUntextured = 1u << 6;  // no material: a soft white dot (a trail: a soft line)
static const uint kFlagTrail = 1u << 7;

// ---- vertex: set 0 resources, set 1 uniforms
[[vk::binding(0, 0)]] Sampler2DShadow vsmAtlas : register(t0, space0) : register(s0, space0);
[[vk::binding(1, 0)]] ByteAddressBuffer sprites : register(t1, space0);
[[vk::binding(2, 0)]] ByteAddressBuffer lights : register(t2, space0);
[[vk::binding(3, 0)]] ByteAddressBuffer lightGrid : register(t3, space0);
[[vk::binding(4, 0)]] ByteAddressBuffer vsmState : register(t4, space0);
[[vk::binding(0, 1)]] ConstantBuffer<ParticleUniforms> uv : register(b0, space1);

// ---- fragment: set 2 resources, set 3 uniforms
[[vk::binding(0, 2)]] Sampler2DArray<float4> vtAlbedo : register(t0, space2) : register(s0, space2);
[[vk::binding(1, 2)]] Sampler2D<float> sceneDepth : register(t1, space2) : register(s1, space2);
[[vk::binding(2, 2)]] ByteAddressBuffer materialData : register(t2, space2);  // MaterialGpu[] then page table
[[vk::binding(0, 3)]] ConstantBuffer<ParticleUniforms> uf : register(b0, space3);

struct ParticleShadows : IShadowMaps
{
    VsmParams vsm() { return uv.vsm; }
    float3 sunDirection() { return uv.sunDirection.xyz; }
    uint resident(uint entry) { return residentPage(uv.vsm, vsmState, entry); }
    float compare(float2 c, float ref) { return vsmAtlas.SampleCmpLevelZero(c, ref); }
};

struct SpriteOut
{
    float4 position : SV_Position;
    float4 color : COLOR;          // rgb * light, alpha
    float4 uvs : UVS;              // this frame's uv, the next frame's
    float3 depthFade : DEPTH;      // view depth, feather, camera fade
    nointerpolation uint2 info : INFO;  // material, flags
    float blend : BLEND;
};

float3 rotateAbout(float3 v, float3 axis, float angle)
{
    const float c = cos(angle), s = sin(angle);
    return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.0 - c));
}

// What lights a sprite at p: the sun (shadowed), the local lights and the sky, as a white surface
// facing the camera would be lit.
float3 lightAt(float3 p, float3 toCamera)
{
    const ParticleShadows sm = {};
    uint level;
    const float distance = length(p - uv.cameraPos.xyz);
    const float shadow = sunShadow(sm, p, uv.sunDirection.xyz, distance, 0.0, level);
    float3 light = uv.sunColor.rgb * (shadow / kPi);
    // The local lights binned at its centre's pixel and depth.
    const float4 clip = mul(uv.viewProj, float4(p, 1.0));
    if (clip.w > 1e-4)
    {
        const float2 ndc = clip.xy / clip.w;
        const uint2 pixel = uint2(clamp(float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * float2(uv.params.xy), 0.0, float2(uv.params.xy) - 1.0));
        uint visited;
        light += shadeLights(lights, lightGrid, uv.lightGrid, pixel, dot(p - uv.cameraPos.xyz, uv.cameraForward.xyz), p, 1.0, 0.0, 1.0,
                             toCamera, toCamera, visited);
    }
    return light + flatAmbient(float3(0.0, 1.0, 0.0)) * uv.paramsF.y;
}

[shader("vertex")]
SpriteOut vs_sprite(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(1, 1), float2(0, 0), float2(1, 1), float2(0, 1) };
    const float2 corner = corners[vid % 6];
    const SpriteGpu s = sprites.Load<SpriteGpu>(iid * kSpriteStride);
    const float3 p = s.positionSize.xyz;
    const float3 vel = s.velocity.xyz;
    const uint flags = s.info.y;
    const uint align = (flags >> kAlignShift) & 3u;
    const float3 toCamera = normalize(uv.cameraPos.xyz - p);
    float width = s.positionSize.w, height = s.velocity.w;
    float roll = s.params.x;
    float3 right = uv.cameraRight.xyz, up = uv.cameraUp.xyz;
    const float speed = length(vel);
    if (align == 1u && speed > 1e-4)
    {
        // Along its velocity, turned about it to face the camera, longer the faster it goes.
        up = vel / speed;
        right = cross(up, toCamera);
        right = dot(right, right) > 1e-8 ? normalize(right) : uv.cameraRight.xyz;
        height += speed * s.params.w;
        roll = 0.0;
    }
    else if (align == 2u)
    {
        up = float3(0.0, 1.0, 0.0);
        right = cross(up, toCamera);
        right = dot(right, right) > 1e-8 ? normalize(right) : uv.cameraRight.xyz;
    }
    else if (align == 3u)
    {
        right = float3(1.0, 0.0, 0.0);
        up = float3(0.0, 0.0, -1.0);
    }
    if ((flags & kFlagFaceVelocity) != 0u && align == 0u && speed > 1e-4)
        roll += atan2(dot(vel, up), dot(vel, right)) - kPi * 0.5;  // its top toward where it moves on screen
    if (roll != 0.0)
    {
        const float3 axis = align == 3u ? float3(0.0, 1.0, 0.0) : normalize(cross(right, up));
        right = rotateAbout(right, axis, roll);
        up = rotateAbout(up, axis, roll);
    }
    const float2 c = corner - 0.5;
    const float3 world = p + right * (c.x * width) + up * (c.y * height);

    SpriteOut o;
    o.position = mul(uv.viewProj, float4(world, 1.0));
    // Flipbook: frame index and the blend into the next.
    const uint cols = max(s.info.z & 0xffu, 1u), rows = max((s.info.z >> 8) & 0xffu, 1u), frames = cols * rows;
    const float f = max(s.params.y, 0.0);
    const uint a = uint(f) % frames, b = (a + 1u) % frames;
    const float2 inFrame = float2(corner.x, 1.0 - corner.y);
    const float2 cell = 1.0 / float2(cols, rows);
    o.uvs = float4((float2(a % cols, a / cols) + inFrame) * cell, (float2(b % cols, b / cols) + inFrame) * cell);
    o.blend = frac(f);
    const float3 light = (flags & kFlagLit) != 0u ? lightAt(p, toCamera) : float3(1.0, 1.0, 1.0);
    o.color = float4(s.color.rgb * light, s.color.a);
    // Camera fade: gone nearer than `near`, in fully by `far` (0: off).
    const float viewDepth = dot(world - uv.cameraPos.xyz, uv.cameraForward.xyz);
    const float fadeNear = float(s.info.w & 0xffffu) * 0.01, fadeFar = float(s.info.w >> 16) * 0.01;
    const float centreDepth = dot(p - uv.cameraPos.xyz, uv.cameraForward.xyz);
    const float cameraFade = fadeFar > fadeNear ? saturate((centreDepth - fadeNear) / (fadeFar - fadeNear)) : 1.0;
    o.depthFade = float3(viewDepth, s.params.z, cameraFade);
    o.info = uint2(s.info.x, flags);
    return o;
}

float4 sampleSprite(MaterialGpu m, float2 coord, float2 dx, float2 dy)
{
    if (m.vtPageTable == 0xffffffffu)
        return 1.0;
    const uint mips = vtMipCount(m);
    const float mip = vtMipLevel(m, dx, dy, uf.paramsF.x);
    const uint m0 = min(uint(mip), mips - 1);
    const float t = m0 + 1 < mips ? frac(mip) : 0.0;
    const float2 wrapped = saturate(coord) * 0.99999;  // (a sheet's frames: clamped, never wrapped)
    const VtPage pa = vtFind(materialData, uf.params.z, m, wrapped, m0);
    const VtPage pb = t > 0.0 ? vtFind(materialData, uf.params.z, m, wrapped, m0 + 1) : pa;
    return lerp(vtAlbedo.SampleLevel(float3(pa.uv, float(pa.layer)), 0.0), vtAlbedo.SampleLevel(float3(pb.uv, float(pb.layer)), 0.0), t);
}

struct SpriteTargets
{
    float4 color : SV_Target0;
    float reactive : SV_Target1;
};

SpriteTargets targets(float4 color, float coverage)
{
    SpriteTargets t;
    t.color = color;
    t.reactive = saturate(coverage * 0.9);
    return t;
}

[shader("fragment")]
SpriteTargets fs_sprite(SpriteOut i)
{
    // Derivatives first, where every pixel of the quad still runs.
    const float2 dxA = ddx(i.uvs.xy), dyA = ddy(i.uvs.xy);
    // No material: a soft dot.
    const float2 fromCentre = i.uvs.xy * 2.0 - 1.0;
    const float falloff = (i.info.y & kFlagTrail) != 0u ? saturate(1.0 - fromCentre.x * fromCentre.x) : saturate(1.0 - dot(fromCentre, fromCentre));
    float4 c = float4(i.color.rgb, i.color.a * falloff);
    if ((i.info.y & kFlagUntextured) == 0u)
    {
        const MaterialGpu m = materialData.Load<MaterialGpu>(i.info.x * kMaterialStride);
        float4 tex = sampleSprite(m, i.uvs.xy, dxA, dyA);
        if (i.blend > 0.0)
            tex = lerp(tex, sampleSprite(m, i.uvs.zw, dxA, dyA), i.blend);
        tex *= m.baseColor;
        c = float4(tex.rgb * i.color.rgb + m.emissive.rgb * m.emissive.w, tex.a * i.color.a);
    }
    // Behind a surface: hidden; near one: faded over the feather (soft particles).
    const float raw = sceneDepth.SampleLevel(i.position.xy / float2(uf.params.xy), 0.0);
    const float sceneView = raw > 0.0 ? uf.cameraPos.w / raw : 1e9;  // reverse-Z, infinite far
    const float gap = sceneView - i.depthFade.x;
    if (gap <= 0.0)
        discard;
    const float soft = i.depthFade.y > 0.0 ? saturate(gap / i.depthFade.y) : 1.0;
    c.a *= soft * i.depthFade.z;
    switch (i.info.y & kBlendMask)
    {
    case 1u:
        return targets(float4(c.rgb * c.a, 0.0), c.a * saturate(max(c.r, max(c.g, c.b))));  // additive
    case 2u:
        return targets(float4(c.rgb * (soft * i.depthFade.z), c.a), c.a);  // premultiplied (the texture's alpha already in its colour)
    default:
        return targets(float4(c.rgb * c.a, c.a), c.a);
    }
}
