#include "app/cook_window.h"

#include "core/log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <mutex>

namespace fx {

namespace {

// "Falcon: cooking Main (140 models): encoding textures 37%", and the bar's fraction.
std::string describe(const std::string& relative, const CookProgress& p, float& fraction)
{
    std::string text = "Falcon: cooking " + std::filesystem::path(relative).stem().string();
    if (p.models)
        text += " (" + std::to_string(p.models) + " model" + (p.models == 1 ? "" : "s") + ")";
    struct Stage {
        const char* name;
        const char* label;
        float from, to;
    };
    static const Stage kStages[] = {{"waiting", "another window is cooking it", 0, 0}, {"import", "reading the models", 0, 0.05f},
                                    {"meshes", "building meshes", 0.05f, 0.1f},          {"textures", "encoding textures", 0.1f, 0.95f},
                                    {"writing", "writing", 0.95f, 1},                    {"installing", "moving it into place", 1, 1},
                                    {"done", "done", 1, 1}};
    fraction = 0;
    for (const Stage& s : kStages)
        if (p.stage == s.name) {
            fraction = s.from + (s.to - s.from) * (p.total ? std::min(1.0f, float(p.done) / float(p.total)) : 0.0f);
            text += std::string(": ") + s.label;
            if (p.stage != "waiting")
                text += " " + std::to_string(int(fraction * 100)) + "%";
        }
    return text;
}

}  // namespace

bool cookSceneWithProgress(const Project& project, const std::string& relative, bool hidden, std::string& error)
{
    // Nothing to cook (or a finished cook to move in): no window.
    if (hidden || !sceneNeedsCook(project, relative)) {
        const bool ok = cookProjectScene(project, relative, error);
        if (!ok && !hidden)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Falcon: cooking failed", error.c_str(), nullptr);
        return ok;
    }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        FX_WARN("cook: no window (%s); cooking without one", SDL_GetError());
        return cookProjectScene(project, relative, error);
    }
    SDL_Window* window = SDL_CreateWindow("Falcon: cooking", 560, 72, 0);
    std::mutex lock;
    CookProgress progress;
    std::atomic<bool> cancel{false};
    std::string cookError;
    auto done = std::async(std::launch::async, [&] {
        return cookProjectScene(
            project, relative, cookError,
            [&](const CookProgress& p) {
                std::lock_guard<std::mutex> l(lock);
                progress = p;
                return !cancel.load();
            });
    });
    std::string title;
    while (done.wait_for(std::chrono::milliseconds(30)) != std::future_status::ready) {
        for (SDL_Event e; SDL_PollEvent(&e);)
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                cancel = true;
        CookProgress p;
        {
            std::lock_guard<std::mutex> l(lock);
            p = progress;
        }
        float fraction = 0;
        if (const std::string text = describe(relative, p, fraction); window && text != title) {
            title = text;
            SDL_SetWindowTitle(window, title.c_str());
        }
        if (SDL_Surface* s = window ? SDL_GetWindowSurface(window) : nullptr) {
            const SDL_PixelFormatDetails* f = SDL_GetPixelFormatDetails(s->format);
            const SDL_Rect frame = {16, s->h / 2 - 8, s->w - 32, 16};
            const SDL_Rect fill = {frame.x + 2, frame.y + 2, int(float(frame.w - 4) * fraction), frame.h - 4};
            SDL_FillSurfaceRect(s, nullptr, SDL_MapRGB(f, nullptr, 0x1e, 0x1f, 0x22));
            SDL_FillSurfaceRect(s, &frame, SDL_MapRGB(f, nullptr, 0x3a, 0x3c, 0x42));
            SDL_FillSurfaceRect(s, &fill, SDL_MapRGB(f, nullptr, 0x4c, 0x8d, 0xf6));
            SDL_UpdateWindowSurface(window);
        }
    }
    const bool ok = done.get();
    if (window)
        SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    error = cookError;
    if (!ok && error != "Cancelled")
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Falcon: cooking failed", error.c_str(), nullptr);
    return ok;
}

}  // namespace fx
