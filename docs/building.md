# Build

Requirements: Visual Studio 2022 (C++), CMake 3.25+, Ninja, Windows SDK (for `dxcompiler.dll`),
the Slang binary release (`FALCON_SLANG_ROOT`, default `%USERPROFILE%/slang-2026.10.2-windows-x86_64`)
and the .NET 10 SDK (C# scripts; the NativeAOT compiler package is restored from NuGet).

```powershell
git submodule update --init --depth 1
./build.ps1 -Preset release     # or debug / profile (profile enables Tracy)
./build/release/bin/falcon.exe
```

On Linux and macOS (Apple silicon, macOS 14+): CMake 3.25+, Ninja, a C++20 compiler (gcc 13+,
clang 17+ or Xcode 16+), a C23 compiler with `#embed` for the UI (gcc 15+ or clang 19+: Ubuntu's
`clang-19`, Homebrew's `llvm` on macOS), the .NET 10 SDK and, on Linux, SDL's X11/Wayland/audio
development packages (the list is in `.github/workflows/build.yml`). `build.sh` fetches Slang into
`~/slang-<version>-<os>-<arch>` and builds the `linux` or `macos` preset into `build/<preset>/`. Steam Audio and the Steamworks SDK
come from `cmake -DDEP=steamaudio -P scripts/fetch.cmake` and
`cmake -DDEP=steamworks -DZIP=steamworks_sdk_165.zip -P scripts/fetch.cmake`. Not there yet: QUIC
(MsQuic) and AMD FSR / NVIDIA DLSS (Direct3D 12 only).

```bash
git submodule update --init --depth 1
./build.sh
./build/linux/bin/falcon --help
```

GitHub Actions builds all three on every push to main (`.github/workflows/build.yml`) and runs
the tests that need no GPU (`scripts/ci_tests.py`); the macOS job also compiles every shader's
MSL with Apple's compiler. GPU tests run on real machines (`ci_tests.py --kind gpu --only <regex>`
picks a set; Mesa's lavapipe under Xvfb works on Linux). The Steamworks SDK needs a Steamworks
partner login, so it isn't in this repository: CI fetches it only with the `STEAMWORKS_TOKEN`
secret, and forks and pull requests build without Steam. A `v*` tag packages the three builds
into a draft GitHub Release.

Like Godot, the engine is one executable, `falcon.exe`: the launcher (no arguments), the game,
the editor (`--editor`) and the cooker (`falcon cook ...`); `falcon --help` lists the modes. SDL is linked in. A build's
`bin/` holds:

```
falcon.exe      the game, the editor and the cooker (with SDL and the UI's Ember linked in)
fsr/            AMD's FSR DLLs and license (when fetched; see below)
dlss/           the DLSS plugin, NVIDIA's DLSS DLL and license (when fetched; see below)
audio/          Valve's Steam Audio (phonon.dll) and its third-party licences (when fetched; see below)
falcon-ui/      the HTML UI bundles (launcher, editor, hud), but in a release build: see below
content/        the engine's content (the material library, the prototype textures), likewise
scripts/        the C# scripts
```

A Windows release build packs `falcon-ui/` and `content/` into `falcon.exe` instead
(`FALCON_EMBED_DATA`, on by default there; `-DFALCON_EMBED_DATA=ON` or `OFF` for any build). After
each link, or when `ui/` or `content/` change, `tools/pack` (`falcon_pack`, in `tools/`) appends
their files to the executable, after the program, where the loader doesn't look
(`engine/core/bundle.h` has the layout), and removes the two folders from `bin/`. falcon.exe is then
about 267 MB: the program is 24 MB and the content 243 MB. `falcon_pack <exe> --list` shows what an
executable carries.
- **The pages** are read from the executable as they load: the editor's page is
  `falcon://ui/editor/index.html`, served by the UI layer (`engine/core/engine_files.h`). There is
  nothing to watch, so a packed page doesn't reload live; debug and profile builds keep the folder
  for that.
- **The content** is copied into projects by ordinary file code, so the editor unpacks it once
  into the user's cache: `%LOCALAPPDATA%/Falcon/Content/<hash of the content>/`
  (`~/.cache/falcon` on Linux, `~/Library/Caches/Falcon` on macOS, `FALCON_CACHE_DIR` for tests).
  It unpacks on a thread from the editor's start, in about 0.7 s the first time a version runs, and
  other versions' copies there are removed. The material library waits for it when first listed.
- **A game build** (File > Build Game) gets the pages in its executable but not the content: its
  `<name>.exe` is about 24 MB.
- Test: `editor_packed` packs a copy of falcon.exe on its own in a folder, loads the editor's page
  from it and lists the Falcon materials it unpacked.

The other folders stay files: Windows loads a DLL only from a file, the vendors' DLLs are signed or
licensed as they are, and `shaderc/` and `scripts/` are programs and assemblies other processes run.

Build-time tools (`falcon_shaderc`, `falcon_schemagen`) and the test tools `falcon_imgdiff` and
`falcon_wavdiff` go to `tools/`, not `bin/`.

The first configure copies SDL into the build tree and applies four patches there; the
submodule itself stays pristine:
- `cmake/patches/sdl3-falcon-external-texture.patch` (from cef-lite, which the UI used to be:
  the patches below were made on top of it);
- `cmake/patches/sdl3-falcon-timestamps.patch`: GPU timestamp queries for D3D12 and Vulkan
  (`SDL3/SDL_falcon_timestamps.h`), which the profiler uses. This is a local patch, not meant
  for upstream SDL;
- `cmake/patches/sdl3-falcon-static.patch`: the two extensions' API macros drop
  `dllimport` / `dllexport`, since SDL is a static library here;
- `cmake/patches/sdl3-falcon-native.patch`: native Direct3D 12 access (device, command list,
  textures; `SDL3/SDL_falcon_native.h`) for AMD FSR. Also local only.

AMD FSR needs a few files from the AMD FSR SDK v2.3.0 (headers and the signed
`amd_fidelityfx_loader_dx12.dll` / `amd_fidelityfx_upscaler_dx12.dll`, about 29 MB):
`powershell -File scripts/fetch-fsr.ps1` puts them in `external/fidelityfx/` (not committed).
The next configure turns `FALCON_FSR` on and stages the DLLs, with AMD's `license.md` (the
DLLs are under its MIT-style terms), in `bin/fsr/`. They load at runtime, so falcon.exe runs
without them and upscales with TAAU instead.

NVIDIA DLSS needs a few files from NVIDIA's DLSS SDK v310.9.16 on GitHub (the NGX headers, the
static `nvsdk_ngx_s.lib` and `nvsdk_ngx_s_dbg.lib`, and `nvngx_dlss.dll`, about 60 MB):
`powershell -File scripts/fetch-dlss.ps1` puts them in `external/dlss/` (not committed: they are
under NVIDIA's license, `external/dlss/LICENSE.txt`). The next configure turns `FALCON_DLSS` on
and builds a plugin, `bin/dlss/falcon_dlss.dll` (`engine/render/dlss_plugin/`), the only thing
that links NVIDIA's library, next to `nvngx_dlss.dll` and NVIDIA's `LICENSE.txt`. falcon.exe has
no NVIDIA code: it loads the plugin when DLSS is first picked, and upscales with TAAU without it.

Spatial audio uses Valve's Steam Audio SDK v4.8.1 (Apache 2.0). `powershell -File
scripts/fetch-steamaudio.ps1` downloads `steamaudio_4.8.1.zip` (181 MB, every platform; or
`-Zip <path>` to use one already downloaded). It checks the zip's SHA-256 and keeps only the
headers, the Windows x64 `phonon.dll` (53 MB) with its import library, and `THIRDPARTY.md`, in
`external/steamaudio/` (not committed). The next configure turns `FALCON_STEAM_AUDIO` on and
stages the DLL and `THIRDPARTY.md` in `bin/audio/`. `phonon.dll` is delay-loaded when audio
starts, so falcon.exe runs without it and pans sounds instead of using the HRTF.

The UI (the launcher, the editor and in-game HTML) is Ember (`external/ember`,
[Tebarem/ember](https://github.com/Tebarem/ember), zlib), an HTML, CSS and JavaScript engine that
draws with SDL_GPU on the engine's own device. It is C23 (`#embed`), which MSVC can't compile, so
`cmake/FalconUi.cmake` builds it as its own CMake project with LLVM's clang for the MSVC ABI (from
`C:/Program Files/LLVM`; `winget install LLVM.LLVM`, 19 or newer) against the engine's patched SDL,
with the static C runtime, and links the static library into falcon.exe: one SDL in the process.
`-DFALCON_EMBER_DIR=<checkout>` builds another Ember tree (to work on Ember beside Falcon). Without
the submodule, or with `-DFALCON_UI=OFF`, the engine builds without UI. The configure step fetches
the three of Ember's own submodules its library needs (SDL_image, SheenBidi, kb) when they are
missing; the others are for its tests and networking.

The level tools use four more submodules, built static with the engine (`cmake/FalconDeps.cmake`):
- Manifold 3.5.4 (Apache-2.0): booleans for cut volumes. Built serial, without its 2D
  CrossSection (Clipper2), its downloads or its Tracy hook.
- MikkTSpace (zlib): tangents (`engine/core/mesh_tangents.h`).
- xatlas (MIT): unwrapping for paint-mask UVs.
- Recast/Detour 1.6.0 (zlib): navigation meshes. Its own CMake file predates CMake 3.5, so the
  libraries are built from their sources.

`falcon_level_test` (in `tools/`) checks them without the engine: ctest runs `level_tangents`,
`level_boolean` (a door and a window cut from a wall, exact volume, identical reruns),
`level_unwrap` and `level_navmesh` (a Detour path around a wall).

`falcon.exe` must not be marked CET compatible (never link it `/CETCOMPAT`): see the
renderer-crash note under UI and editor.
