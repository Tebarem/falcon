#include "level/level_mask.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx::level {

namespace {

const char* const kDigits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const uint8_t* b, size_t n)
{
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = uint32_t(b[i]) << 16 | (i + 1 < n ? uint32_t(b[i + 1]) << 8 : 0u) | (i + 2 < n ? b[i + 2] : 0u);
        out += kDigits[v >> 18 & 63];
        out += kDigits[v >> 12 & 63];
        out += i + 1 < n ? kDigits[v >> 6 & 63] : '=';
        out += i + 2 < n ? kDigits[v & 63] : '=';
    }
    return out;
}

bool unbase64(std::string_view text, std::vector<uint8_t>& out)
{
    out.clear();
    uint32_t acc = 0, bits = 0;
    for (const char c : text) {
        if (c == '=' || c == '\n' || c == '\r')
            continue;
        const char* at = std::strchr(kDigits, c);
        if (!at || !c)
            return false;
        acc = acc << 6 | uint32_t(at - kDigits);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t(acc >> bits));
        }
    }
    return true;
}

}  // namespace

std::string maskToText(const std::vector<uint8_t>& rgba)
{
    if (rgba.size() != kMaskBytes)
        return {};
    std::string text;
    SDL_Surface* s = SDL_CreateSurfaceFrom(int(kMaskSize), int(kMaskSize), SDL_PIXELFORMAT_RGBA32, const_cast<uint8_t*>(rgba.data()),
                                           int(kMaskSize * 4));
    SDL_IOStream* io = s ? SDL_IOFromDynamicMem() : nullptr;
    if (io && SDL_SavePNG_IO(s, io, false)) {
        const auto* bytes = static_cast<const uint8_t*>(SDL_GetPointerProperty(SDL_GetIOProperties(io), SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, nullptr));
        if (bytes)
            text = base64(bytes, size_t(SDL_GetIOSize(io)));
    }
    if (io)
        SDL_CloseIO(io);
    SDL_DestroySurface(s);
    return text;
}

bool maskFromText(std::string_view text, std::vector<uint8_t>& rgba)
{
    rgba.clear();
    std::vector<uint8_t> png;
    if (!unbase64(text, png) || png.empty())
        return false;
    SDL_IOStream* io = SDL_IOFromConstMem(png.data(), png.size());
    SDL_Surface* loaded = io ? SDL_LoadPNG_IO(io, true) : nullptr;
    SDL_Surface* s = loaded ? SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32) : nullptr;
    SDL_DestroySurface(loaded);
    const bool ok = s && s->w == int(kMaskSize) && s->h == int(kMaskSize);
    if (ok) {
        rgba.resize(kMaskBytes);
        for (uint32_t y = 0; y < kMaskSize; ++y)
            std::memcpy(&rgba[size_t(y) * kMaskSize * 4], static_cast<const uint8_t*>(s->pixels) + size_t(y) * size_t(s->pitch), kMaskSize * 4);
    }
    SDL_DestroySurface(s);
    return ok;
}

std::vector<uint8_t> maskFilled(const std::vector<uint8_t>* from, uint32_t channel, float value)
{
    std::vector<uint8_t> out = from && from->size() == kMaskBytes ? *from : std::vector<uint8_t>(kMaskBytes, 0);
    const uint8_t v = uint8_t(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    for (size_t i = std::min<uint32_t>(channel, 3u); i < out.size(); i += 4)
        out[i] = v;
    return out;
}

}  // namespace fx::level
