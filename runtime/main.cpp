// falcon.exe: the engine's one executable.
//   falcon                      the launcher: recent projects, New / Open project
//   falcon [options]            the game (--scene x.fpak ...; --editor for the editor)
//   falcon cook <in> <out> ...  the cooker: glTF -> .fpak (or --empty <out>)
// falcon --help lists the options.

#include "app/engine.h"
#include "core/boot.h"
#include "core/dmath.h"
#include "cook_main.h"
#include "vfx/vfx_selftest.h"

#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "cook") == 0)
        return cookMain(argc - 1, argv + 1);
    if (argc > 1 && std::strcmp(argv[1], "vfx-test") == 0)
        return fx::vfxSelfTest();
    if (argc > 1 && std::strcmp(argv[1], "dmath-hash") == 0) {  // (the deterministic math's bits: sim_dmath_*)
        fx::dmath::setupFloatingPoint();
        std::printf("dmath self-hash %016llx\n", (unsigned long long)fx::dmath::selfHash());
        return 0;
    }
    if (argc > 1 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        std::printf("usage:\n"
                    "  falcon                                         the launcher: open or create a project\n"
                    "  falcon --editor --project <dir> [--scene s]    the editor on a project (s: relative to it)\n"
                    "  falcon [--scene x.fpak] [--editor] [options]   run the game or the editor (docs/runtime-flags.md)\n"
                    "  falcon --preview jobs.json                     render asset previews to PNGs (engine/app/preview.h)\n"
                    "  falcon cook <in.gltf|.glb> <out.fpak> [--force] [--verify] [--threads N] [--vt-max-size N] [--graph g.json]\n"
                    "              [--library assets.gltf] [--material m.fmat] [--source-root dir] [--rocks]\n"
                    "  falcon cook --empty <out.fpak> [--rocks]       an empty scene (the built-in shapes; --rocks adds the rocks)\n"
                    "  falcon vfx-test                                check the particle values and components\n"
                    "  falcon dmath-hash                              the deterministic math's self-hash\n");
        return 0;
    }
    fx::boot::mark("process main");
    fx::EngineConfig config;
    if (!fx::parseEngineArgs(argc, argv, config))
        return 2;
    return fx::runEngine(config);
}
