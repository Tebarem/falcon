// Falcon post effects (R3): what a project's post effect reads and writes. An effect is a compute
// shader in the project's Shaders/ (compiled by the editor into Cache/Shaders/), run on the frame
// each frame by a script's PostEffect:
//
//   import falcon_post;
//   struct Params { float Amount; float Gain; float2 Unused; };  // its own values (C#: Set by name),
//   [[vk::binding(0, 2)]] ConstantBuffer<Params> params : register(b0, space2);  // in rows of 16 bytes
//
//   [shader("compute")]
//   [numthreads(8, 8, 1)]
//   void main(uint3 id : SV_DispatchThreadID)
//   {
//       if (falconOutside(id.xy))
//           return;
//       const float4 c = falconSource.SampleLevel(falconUv(id.xy), 0.0);
//       falconTarget[id.xy] = c * params.Gain;  // falconTargetLdr after the tonemap
//   }
//
// Before the tonemap (PostStage.BeforeTonemap) the frame is linear HDR (after TAA or the upscaler,
// at the display's size) and the effect writes falconTarget; after it (AfterTonemap) the frame is
// the display's (sRGB-encoded 0..1, before FXAA) and the effect writes falconTargetLdr.

// The frame so far (bilinear, clamped).
[[vk::binding(0, 0)]] Sampler2D<float4> falconSource : register(t0, space0) : register(s0, space0);
// Its depth: reverse-Z, infinite far (1 near, 0 far), at the render size (falconViewDistance).
[[vk::binding(1, 0)]] Sampler2D<float> falconDepth : register(t1, space0) : register(s1, space0);
// Where the effect writes: before the tonemap, and after it.
[[vk::binding(0, 1)]] [format("rgba16f")] RWTexture2D<float4> falconTarget : register(u0, space1);
[[vk::binding(0, 1)]] [format("rgba8")] RWTexture2D<float4> falconTargetLdr : register(u0, space1);

struct FalconFrame
{
    float4 size;  // width, height, 1 / width, 1 / height
    float4 time;  // seconds, this frame's seconds, the frame's number
    float4 proj;  // the projection's P00, P11 and near distance
};
[[vk::binding(1, 2)]] ConstantBuffer<FalconFrame> falconFrame : register(b1, space2);

// A pixel's texture coordinate, and whether it is past the frame's edge (the last thread groups).
float2 falconUv(uint2 pixel) { return (float2(pixel) + 0.5) * falconFrame.size.zw; }
bool falconOutside(uint2 pixel) { return pixel.x >= uint(falconFrame.size.x) || pixel.y >= uint(falconFrame.size.y); }

// How far from the camera (along its view) the surface at a texture coordinate is, in metres.
float falconViewDistance(float2 uv) { return falconFrame.proj.z / max(falconDepth.SampleLevel(uv, 0.0), 1e-12); }
