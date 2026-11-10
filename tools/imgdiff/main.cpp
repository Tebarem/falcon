// falcon_imgdiff: compares two PNGs for golden-image tests.
//   falcon_imgdiff a.png b.png [--tolerance N] [--max-pixels N] [--min-pixels N] [--out diff.png]
//   falcon_imgdiff img.png --probe x y         prints the pixel (r g b a)
//   falcon_imgdiff img.png --dominant x y r|g|b  exit 0 when that channel is the pixel's largest
// A pixel differs when any channel differs by more than the tolerance (0-255).
// Exit code 0 when at most max-pixels and at least min-pixels differ (a minimum checks that an
// effect, e.g. anti-aliasing, changed the image at all).

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: falcon_imgdiff a.png b.png [--tolerance N] [--max-pixels N] [--min-pixels N]\n");
        return 2;
    }
    int tolerance = 2;
    long maxPixels = 0, minPixels = 0;
    const char* outPath = nullptr;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--tolerance")) tolerance = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--max-pixels")) maxPixels = std::atol(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--min-pixels")) minPixels = std::atol(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--out")) outPath = argv[i + 1];
    }
    if (!std::strcmp(argv[2], "--dominant") && argc >= 6) {
        SDL_Surface* img = SDL_ConvertSurface(SDL_LoadPNG(argv[1]), SDL_PIXELFORMAT_RGBA32);
        if (!img)
            return 2;
        const int x = std::atoi(argv[3]), y = std::atoi(argv[4]);
        const Uint8* p = static_cast<const Uint8*>(img->pixels) + y * img->pitch + x * 4;
        const int c = argv[5][0] == 'r' ? 0 : argv[5][0] == 'g' ? 1 : 2;
        std::printf("falcon_imgdiff: %d %d %d at %d, %d (want %s largest)\n", p[0], p[1], p[2], x, y, argv[5]);
        return p[c] > p[(c + 1) % 3] && p[c] > p[(c + 2) % 3] ? 0 : 1;
    }
    if (!std::strcmp(argv[2], "--probe") && argc >= 5) {  // falcon_imgdiff img.png --probe x y
        SDL_Surface* img = SDL_ConvertSurface(SDL_LoadPNG(argv[1]), SDL_PIXELFORMAT_RGBA32);
        const int x = std::atoi(argv[3]), y = std::atoi(argv[4]);
        const Uint8* p = static_cast<const Uint8*>(img->pixels) + y * img->pitch + x * 4;
        std::printf("%d %d %d %d\n", p[0], p[1], p[2], p[3]);
        return 0;
    }
    SDL_Surface* a = SDL_LoadPNG(argv[1]);
    SDL_Surface* b = SDL_LoadPNG(argv[2]);
    if (!a || !b) {
        std::fprintf(stderr, "falcon_imgdiff: %s\n", SDL_GetError());
        return 2;
    }
    SDL_Surface* ca = SDL_ConvertSurface(a, SDL_PIXELFORMAT_RGBA32);
    SDL_Surface* cb = SDL_ConvertSurface(b, SDL_PIXELFORMAT_RGBA32);
    if (!ca || !cb || ca->w != cb->w || ca->h != cb->h) {
        std::fprintf(stderr, "falcon_imgdiff: images differ in size or failed to convert\n");
        return 1;
    }
    // Diff image: differing pixels in red over a dimmed copy of the first image.
    SDL_Surface* diff = outPath ? SDL_DuplicateSurface(ca) : nullptr;
    long differing = 0;
    int maxDiff = 0;
    for (int y = 0; y < ca->h; ++y) {
        const Uint8* ra = static_cast<const Uint8*>(ca->pixels) + y * ca->pitch;
        const Uint8* rb = static_cast<const Uint8*>(cb->pixels) + y * cb->pitch;
        for (int x = 0; x < ca->w; ++x) {
            int d = 0;
            for (int c = 0; c < 3; ++c)
                d = SDL_max(d, SDL_abs(int(ra[x * 4 + c]) - int(rb[x * 4 + c])));
            maxDiff = SDL_max(maxDiff, d);
            differing += d > tolerance;
            if (diff) {
                Uint8* px = static_cast<Uint8*>(diff->pixels) + y * diff->pitch + x * 4;
                if (d > tolerance) {
                    px[0] = 255;
                    px[1] = px[2] = 0;
                } else {
                    px[0] = Uint8(px[0] / 4);
                    px[1] = Uint8(px[1] / 4);
                    px[2] = Uint8(px[2] / 4);
                }
            }
        }
    }
    if (diff) {
        SDL_SavePNG(diff, outPath);
        SDL_DestroySurface(diff);
    }
    const long total = long(ca->w) * ca->h;
    std::printf("falcon_imgdiff: %ld / %ld pixels differ by more than %d (%.4f%%), max channel diff %d\n", differing,
                total, tolerance, 100.0 * double(differing) / double(total), maxDiff);
    return differing <= maxPixels && differing >= minPixels ? 0 : 1;
}
