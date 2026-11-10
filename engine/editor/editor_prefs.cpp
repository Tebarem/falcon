#include "editor/editor_prefs.h"

#include "core/log.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <fstream>

namespace fx {

bool EditorPrefs::load()
{
    if (char* dir = SDL_GetPrefPath("Falcon", "Editor")) {
        path_ = std::string(dir) + "editor.json";
        SDL_free(dir);
    }
    if (path_.empty())
        return false;
    size_t size = 0;
    void* text = SDL_LoadFile(path_.c_str(), &size);
    if (!text)
        return false;
    try {
        root_ = Json::parse(std::string_view(static_cast<const char*>(text), size));
        if (!root_.isObject())
            root_ = Json::makeObject();
    } catch (const std::exception& e) {
        FX_WARN("editor: %s is malformed (%s); starting from defaults", path_.c_str(), e.what());
        root_ = Json::makeObject();
    }
    SDL_free(text);
    return true;
}

void EditorPrefs::save(bool force)
{
    if (!dirty_ || path_.empty())
        return;
    const uint64_t now = SDL_GetTicksNS();
    if (!force && now - lastSaveNs_ < 1'000'000'000ull)
        return;
    lastSaveNs_ = now;
    dirty_ = false;
    // Write-then-rename: a crash never leaves a truncated file.
    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << root_.dump() << '\n';
        if (!out) {
            FX_WARN("editor: cannot write %s", tmp.c_str());
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path_, ec);
    if (ec)
        FX_WARN("editor: cannot replace %s (%s)", path_.c_str(), ec.message().c_str());
}

}  // namespace fx
