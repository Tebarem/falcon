# SDL_GPU notes (SDL 3.4.18 + cef-lite patch)

Facts verified against SDL source or measured on hardware (AMD RX 9060 XT, Windows 11).
The renderer design depends on them.

## Shader rules (enforced by `falcon_shaderc`)

- **Storage buffers must be `(RW)ByteAddressBuffer`** with `Load<T>`/`Store<T>`.
  On D3D12, SDL always creates raw views (`R32_TYPELESS` + `RAW` flag). A `StructuredBuffer`
  read through a raw view returns garbage (seen as an empty frame).
- Binding sets: vertex `0`/`1`, fragment `2`/`3`, compute `0` (RO) / `1` (RW) / `2` (uniforms).
  The HLSL space equals the SPIR-V set. Indices are contiguous per entry point, in the order
  samplers → storage textures → storage buffers.
- Only combined samplers (`Sampler2D`…), with no resource arrays and no loose uniforms.
- Buffer and cbuffer element layouts are compared between SPIR-V and DXIL at build time.
  Avoid `float3` members and pad to 16 bytes.

- **Integer textures read in compute must be read-only storage textures (`Texture2D<uint>`),
  not samplers.** SDL refuses `SAMPLER` usage for R32_UINT on D3D12, because integer formats
  have no filtering support. A read-only storage texture is an SRV on D3D12 and a
  `SAMPLED_IMAGE` on Vulkan (SDL declares it that way on purpose), so a plain `Texture2D` matches both.
- **Slang bug: `Sampler2D.Load` / `operator[]` produce invalid HLSL.** Slang passes the
  sampler into `Texture2D.Load`, which DXC rejects. Fetch float textures through combined
  samplers with a point-sampled `SampleLevel` at the texel center instead (`fetchFloat` in
  `scene_types.slang`).

- BC7 (sRGB and UNORM) and BC5 2D arrays of 136×136 with 1024+ layers work as the virtual
  texture physical cache on both backends. `SDL_UploadToGPUTexture` takes `pixels_per_row` /
  `rows_per_layer` in texels for block formats, and `layer` selects the array slice.

- `SV_ClipDistance` outputs must be separate scalars (`SV_ClipDistance0..3`). A `float4`
  is rejected by Slang for SPIR-V, and a `float[4]` fails DXIL signature packing.
- Comparison samplers (`Sampler2DShadow` + `enable_compare`) work through SDL on both backends.
  A D32 depth target with `SAMPLER` usage serves as the shadow atlas.
- Slang module constants are global across imports: `vt.kPageTexels` and `vsm.kPageTexels`
  are ambiguous, so give module constants distinct names.

## Binding pitfalls (D3D12 debug layer)

- **Never bind one buffer to two slots of the same pass.** SDL transitions a buffer once per
  binding, so a duplicate binding causes state-mismatch errors (`RESOURCE_MANIPULATION ERROR #527`).
  Fill unused slots below a used one with some other buffer.
- **An indirect-args buffer cannot be written in the same pass that consumes it.** Atomics
  go into a separate `counters` buffer, and tiny args kernels convert them into the `args` buffer.

## Vulkan device options

- `SV_PrimitiveID` in a fragment shader requires the `geometryShader` feature on Vulkan.
  SPIR-V 1.5 also needs a Vulkan 1.2 instance. Both are requested through
  `SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER` (SDL 3.4+), see `gpu/device.cpp`.

## Metal

Not yet run on Apple hardware: compiled by Apple's `metal` in CI (`metal-shaders` job), nothing more.

- **The MSL is made from the SPIR-V** by SPIRV-Cross in `falcon_shaderc`, not by Slang's own Metal
  target. The SPIR-V is what Vulkan already runs, and SPIRV-Cross pads MSL structs to the
  SPIR-V offsets, so the SPIR-V/DXIL layout check covers Metal too.
- **Indices are set per resource** in SDL's Metal order (`SDL_gpu.h`): `[[texture]]` sampled,
  then read-only storage, then read-write storage; `[[sampler]]` at the sampled texture's index;
  `[[buffer]]` uniforms, then read-only storage, then read-write storage buffers. Vertex buffers
  start at `[[buffer(14)]]` (`[[stage_in]]`). A resource without an index fails the build.
- MSL 3.0, macOS 14+, Apple silicon. SDL compiles MSL source at shader creation
  (`newLibraryWithSource`, default options); the entry point is SPIRV-Cross's `main0`.
- Limits on Apple GPUs to keep in mind: 16 samplers per stage (SDL_GPU's own limit everywhere),
  31 buffers per stage including vertex buffers from 14, 2048 texture array layers.

## Synchronization

- **No barriers between dispatches inside one compute pass** (`D3D12_DispatchCompute` and
  `VULKAN_DispatchCompute` record only the dispatch). Independent dispatches in a pass overlap,
  which is what we want for GPU occupancy. A dependent dispatch needs a new compute pass;
  SDL transitions resources at pass boundaries.
- Read-write bindings are set at `SDL_BeginGPUComputePass`. Use `cycle = true` for per-frame
  buffers so the GPU never waits on the previous frame (no write-after-read stall).
- No async compute queue, no timestamp queries, and no indirect-count draws. Draw counts
  are set by the CPU, while args (vertex/index counts) can be GPU-written.

## Features

- `SV_ClipDistance` works on both backends. Vulkan requires `shaderClipDistance`
  (SDL rejects devices without it). This validates the VSM page-clip raster plan.
- R32_UINT color target and R32_UINT simultaneous read/write storage, RG32_UINT color target,
  BC7, BC5 and D32 are all supported on the test GPU (logged as `gpu caps:` at boot).
- No 64-bit atomics exposed, so no Nanite-style 64-bit visibility software raster.
- RGBA16F and R16F work as compute storage-write + sampler on both backends (the HDR chain).
- As with buffers, bind each texture to one slot per pass. Bind only the slots the entry
  point actually uses.
- `pow(0, 0)` is NaN, and the tonemapper turns NaN into black. Guard power-0 cases
  explicitly (AO off).

## Boot cost (release build, warm)

| phase | D3D12 | Vulkan |
|---|---|---|
| process start → `main` | ~18 ms | ~18 ms |
| `SDL_Init(VIDEO)` + hidden window | ~40 ms | ~40 ms |
| `SDL_CreateGPUDevice` (worker thread) | ~500 ms | ~190 ms |
| `SDL_ClaimWindowForGPUDevice` | ~11 ms | ~270 ms |
| pipelines + first frame | ~15 ms | ~20 ms |
| **first frame submitted** | **~575 ms** | **~500 ms** |

- A raw `D3D12CreateDevice` costs 260–340 ms on this driver. SDL's `D3D12_PrepareDriver` also
  creates and destroys a full probe device whenever DXIL shaders are requested, so the driver
  initializes twice. Removing that needs an SDL change. Not done here: SDL's contribution
  policy disallows AI-written code.
- SDL's Vulkan backend passes no `VkPipelineCache`, so pipelines rely on the driver's own cache.
- Tracy's static init adds ~220 ms before `main` in `profile` builds only. Shipping builds
  have no Tracy.
- Device creation starts on a worker thread right after `SDL_Init`. Later boot work
  (asset mapping, CEF subprocess, CLR) belongs inside that window, where it costs nothing.
