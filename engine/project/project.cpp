#include "project/project.h"

#include "project/libraries.h"

#include "app/process.h"
#include "assets/material_asset.h"
#include "core/json.h"
#include "core/log.h"
#include "core/platform.h"
#include "editor/scene_edits.h"
#include "project/game_scripts.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fx {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxRecent = 20;

std::string normalizedPath(const fs::path& p)
{
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    std::string s = (ec ? p : a).lexically_normal().generic_string();
    while (s.size() > 1 && s.back() == '/')
        s.pop_back();
    return s;
}

bool readText(const std::string& path, std::string& out)
{
    size_t size = 0;
    void* text = SDL_LoadFile(path.c_str(), &size);
    if (!text)
        return false;
    out.assign(static_cast<const char*>(text), size);
    SDL_free(text);
    return true;
}

// Written beside, then renamed over (a crash leaves the old file).
bool writeText(const std::string& path, const std::string& text)
{
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.write(text.data(), std::streamsize(text.size())))
            return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

}  // namespace

std::string absolutePath(const std::string& path) { return normalizedPath(path); }

std::string fileSafeName(const std::string& name)
{
    std::string out;
    for (const char c : name)
        if (std::isalnum(uint8_t(c)) || c == ' ' || c == '-' || c == '_')
            out += c;
    const size_t a = out.find_first_not_of(' '), b = out.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : out.substr(a, b - a + 1);
}

bool loadProject(const std::string& path, Project& out, std::string& error)
{
    fs::path p(path);
    std::error_code ec;
    if (fs::is_directory(p, ec))
        p /= "project.falcon";
    std::string text;
    if (!readText(p.string(), text)) {
        error = "No project here (no project.falcon in " + normalizedPath(p.parent_path()) + ")";
        return false;
    }
    try {
        const Json root = Json::parse(text);
        out = {};
        out.dir = normalizedPath(p.parent_path());
        out.name = root.getString("name", p.parent_path().filename().string().c_str());
        out.startupScene = root.getString("startupScene");
        if (const Json* s = root.get("scenes"))
            for (const Json& j : s->array)
                if (j.type == Json::Type::String)
                    out.scenes.push_back(j.str);
        if (const Json* r = root.get("rendering"); r && r->isObject()) {
            out.lodPixels = std::clamp(float(r->getNumber("lodPixels", out.lodPixels)), 0.0f, 256.0f);
            out.cullPixels = std::clamp(float(r->getNumber("cullPixels", out.cullPixels)), 0.0f, 64.0f);
            out.terrainPixels = std::clamp(float(r->getNumber("terrainPixels", out.terrainPixels)), 0.1f, 64.0f);
            out.detailDistance = std::clamp(float(r->getNumber("detailDistance", out.detailDistance)), 0.0f, 1000.0f);
        }
        if (const Json* a = root.get("audio"); a && a->isObject()) {
            static const char* const kBuses[5] = {"sfx", "physics", "ambience", "music", "ui"};
            out.audio.master = std::clamp(float(a->getNumber("master", out.audio.master)), 0.0f, 4.0f);
            for (int b = 0; b < 5; ++b)
                out.audio.buses[b] = std::clamp(float(a->getNumber(kBuses[b], out.audio.buses[b])), 0.0f, 4.0f);
            out.audio.hrtf = a->getBool("hrtf", out.audio.hrtf);
            out.audio.reflections = uint32_t(std::clamp(a->getNumber("reflections", out.audio.reflections), 0.0, 3.0));
            out.audio.reverbType = uint32_t(std::clamp(a->getNumber("reverbType", out.audio.reverbType), 0.0, 2.0));
            out.audio.reverb = std::clamp(float(a->getNumber("reverb", out.audio.reverb)), 0.0f, 4.0f);
            out.audio.physicsSounds = a->getBool("physicsSounds", out.audio.physicsSounds);
            out.audio.variation = a->getBool("variation", out.audio.variation);
            out.audio.waterSounds = a->getBool("waterSounds", out.audio.waterSounds);
        }
        if (const Json* v = root.get("navigation"); v && v->isObject()) {
            level::NavSettings& ns = out.navigation;
            ns.agentHeight = std::clamp(float(v->getNumber("agentHeight", ns.agentHeight)), 0.2f, 10.0f);
            ns.agentRadius = std::clamp(float(v->getNumber("agentRadius", ns.agentRadius)), 0.05f, 5.0f);
            ns.agentClimb = std::clamp(float(v->getNumber("agentClimb", ns.agentClimb)), 0.0f, 5.0f);
            ns.maxSlope = std::clamp(float(v->getNumber("maxSlope", ns.maxSlope)), 0.0f, 89.0f);
            ns.cellSize = std::clamp(float(v->getNumber("cellSize", ns.cellSize)), 0.05f, 2.0f);
            ns.cellHeight = std::clamp(float(v->getNumber("cellHeight", ns.cellHeight)), 0.02f, 1.0f);
            out.navAutoBake = v->getBool("autoBake", out.navAutoBake);
        }
        if (const Json* v = root.get("level"); v && v->isObject()) {
            out.levelDefaultMaterial = v->getString("defaultMaterial", out.levelDefaultMaterial.c_str());
            out.prototypeInstalled = v->getBool("prototype", out.prototypeInstalled);
        }
        if (const Json* n = root.get("network"); n && n->isObject()) {
            NetworkSettings& ns = out.network;
            ns.preset = n->getString("preset", ns.preset.c_str());
            ns.mode = n->getString("mode", ns.mode.c_str()) == std::string("rollback") ? "rollback" : "authoritative";
            ns.sendRate = uint32_t(std::clamp(n->getNumber("sendRate", ns.sendRate), 1.0, 60.0));
            ns.bandwidthKbps = uint32_t(std::clamp(n->getNumber("bandwidthKbps", ns.bandwidthKbps), 16.0, 100000.0));
            ns.maxPlayers = uint32_t(std::clamp(n->getNumber("maxPlayers", ns.maxPlayers), 0.0, 100000.0));
            ns.interpolationMs = float(std::clamp(n->getNumber("interpolationMs", ns.interpolationMs), 0.0, 1000.0));
            ns.inputDelay = uint32_t(std::clamp(n->getNumber("inputDelay", ns.inputDelay), 0.0, 8.0));
            ns.rollbackWindow = uint32_t(std::clamp(n->getNumber("rollbackWindow", ns.rollbackWindow), 4.0, 120.0));
            ns.port = uint32_t(std::clamp(n->getNumber("port", ns.port), 1.0, 65535.0));
        }
        if (const Json* st = root.get("steam"); st && st->isObject())
            out.steamAppId = uint32_t(std::clamp(st->getNumber("appId", 0.0), 0.0, 4294967295.0));
        if (const Json* st = root.get("steam"); st && st->isObject()) {
            out.steamDepotId = uint32_t(std::clamp(st->getNumber("depotId", 0.0), 0.0, 4294967295.0));
            out.steamBranch = st->getString("branch", "");
            out.steamRestartIfNecessary = st->getBool("restartIfNecessary", true);
        }
    } catch (const std::exception& e) {
        error = "project.falcon is malformed (" + std::string(e.what()) + ")";
        return false;
    }
    if (out.startupScene.empty() && !out.scenes.empty())
        out.startupScene = out.scenes.front();
    if (out.startupScene.empty()) {
        error = "The project has no scenes";
        return false;
    }
    return true;
}

bool applyNetworkPreset(NetworkSettings& s, const std::string& preset)
{
    NetworkSettings p;
    p.preset = preset;
    if (preset == "competitive") {
        p.mode = "authoritative", p.sendRate = 30, p.bandwidthKbps = 512, p.maxPlayers = 64, p.interpolationMs = 100.0f;
    } else if (preset == "large") {
        p.mode = "authoritative", p.sendRate = 20, p.bandwidthKbps = 256, p.maxPlayers = 1000, p.interpolationMs = 150.0f;
    } else if (preset == "mmo") {
        p.mode = "authoritative", p.sendRate = 10, p.bandwidthKbps = 128, p.maxPlayers = 2000, p.interpolationMs = 200.0f;
    } else if (preset == "rollback") {
        p.mode = "rollback", p.maxPlayers = 8, p.inputDelay = 2, p.rollbackWindow = 32;
    } else {
        return false;
    }
    p.port = s.port;
    s = p;
    return true;
}

std::string networkJson(const NetworkSettings& n)
{
    JsonWriter w;
    w.beginObject().value("mode", n.mode).value("sendRate", n.sendRate).value("bandwidthKbps", n.bandwidthKbps)
        .value("maxPlayers", n.maxPlayers).value("interpolationMs", double(n.interpolationMs)).value("inputDelay", n.inputDelay)
        .value("rollbackWindow", n.rollbackWindow).value("port", n.port).endObject();
    return w.str();
}

bool saveProject(const Project& project, std::string& error)
{
    JsonWriter w;
    w.beginObject().value("format", 1u).value("name", project.name).value("startupScene", project.startupScene);
    w.beginArray("scenes");
    for (const std::string& s : project.scenes)
        w.value(nullptr, s);
    w.endArray();
    w.beginObject("rendering").value("lodPixels", double(project.lodPixels)).value("cullPixels", double(project.cullPixels))
        .value("terrainPixels", double(project.terrainPixels)).value("detailDistance", double(project.detailDistance)).endObject();
    const AudioSettings& a = project.audio;
    w.beginObject("audio").value("master", double(a.master)).value("sfx", double(a.buses[0])).value("physics", double(a.buses[1]))
        .value("ambience", double(a.buses[2])).value("music", double(a.buses[3])).value("ui", double(a.buses[4]))
        .value("hrtf", a.hrtf).value("reflections", a.reflections).value("reverbType", a.reverbType).value("reverb", double(a.reverb))
        .value("physicsSounds", a.physicsSounds)
        .value("variation", a.variation)
        .value("waterSounds", a.waterSounds)
        .endObject();
    if (project.steamAppId)
        w.beginObject("steam").value("appId", project.steamAppId).value("depotId", project.steamDepotId).value("branch", project.steamBranch)
            .value("restartIfNecessary", project.steamRestartIfNecessary).endObject();
    const NetworkSettings& n = project.network;
    const level::NavSettings& nav = project.navigation;
    w.beginObject("navigation").value("agentHeight", double(nav.agentHeight)).value("agentRadius", double(nav.agentRadius))
        .value("agentClimb", double(nav.agentClimb)).value("maxSlope", double(nav.maxSlope)).value("cellSize", double(nav.cellSize))
        .value("cellHeight", double(nav.cellHeight)).value("autoBake", project.navAutoBake).endObject();
    w.beginObject("level").value("defaultMaterial", project.levelDefaultMaterial).value("prototype", project.prototypeInstalled).endObject();
    w.beginObject("network").value("preset", n.preset).value("mode", n.mode).value("sendRate", n.sendRate)
        .value("bandwidthKbps", n.bandwidthKbps).value("maxPlayers", n.maxPlayers).value("interpolationMs", double(n.interpolationMs))
        .value("inputDelay", n.inputDelay).value("rollbackWindow", n.rollbackWindow).value("port", n.port).endObject();
    w.endObject();
    // Keys this doesn't know (the libraries' list: project/libraries.h) are kept as they are.
    Json root = Json::parse(w.str());
    std::string old;
    if (readText(project.file(), old))
        try {
            const Json was = Json::parse(old);
            for (const auto& [key, value] : was.object)
                if (!root.get(key.c_str()))
                    root.set(key, value);
        } catch (const std::exception&) {
        }
    if (!writeText(project.file(), root.dump(2) + "\n")) {
        error = "Cannot write " + project.file();
        return false;
    }
    return true;
}

bool cookEmptyScene(const std::string& fpakPath, std::string& error)
{
    FX_INFO("project: cooking an empty scene: %s", fpakPath.c_str());
    const int code = runFalcon({"cook", "--empty", fpakPath, "--rocks"});
    if (code != 0) {
        error = "Cooking " + fpakPath + " failed (" + std::to_string(code) + ")";
        return false;
    }
    return true;
}

namespace {

// Scenes/Main.fpak -> Scenes_Main.fpak: its files in Cache/.
std::string cacheName(const std::string& relative)
{
    std::string name = relative;
    std::replace(name.begin(), name.end(), '/', '_');
    return name;
}

std::string stampFile(const std::string& projectDir, const std::string& relative)
{
    return projectDir + "/Cache/Stamps/" + cacheName(relative) + ".models";
}

std::string fileStamp(const fs::path& p)
{
    std::error_code ec;
    const int64_t written = int64_t(fs::last_write_time(p, ec).time_since_epoch().count());  // (libc++ counts in __int128)
    return std::to_string(fs::file_size(p, ec)) + "\t" + std::to_string(written);
}

// The meshes scene `relative` cooks displaced (tools/cook/build_displace.cpp): each mesh of its
// saved entities (prefabs' too) with a material asset that has height displacement, and the
// sphere with every such material (its preview).
std::vector<std::pair<std::string, std::string>> displacedMeshes(const std::string& projectDir, const std::string& relative)
{
    std::set<std::pair<std::string, std::string>> pairs;
    std::set<std::string> displaced;
    for (const std::string& m : projectMaterials(projectDir)) {
        MaterialAsset asset;
        std::string error;
        if (readMaterialAsset(projectDir + "/" + m, asset, error) && asset.displaced()) {
            displaced.insert(m);
            pairs.insert({"sphere", m});
        }
    }
    if (!displaced.empty()) {
        SceneEdits edits;
        if (edits.load(SceneEdits::pathFor((projectDir + "/" + relative).c_str()))) {
            edits.expandPrefabs(projectDir);
            for (const EntityEdit& e : edits.created)
                if (e.kind == "primitive" && displaced.contains(e.material))
                    pairs.insert({e.primitive, e.material});
        }
    }
    return {pairs.begin(), pairs.end()};
}

// One line per model (its path, size and date), per material asset with textures (its path and
// its textures, theirs: its factors are read at load, so changing them needs no cook, and one
// without textures shows through a spare slot; its height map; a displaced one's amounts), and
// per mesh the scene cooks displaced.
std::string modelsStamp(const std::string& projectDir, const std::string& relative)
{
    std::string stamp;
    for (const std::string& m : projectModels(projectDir))
        stamp += m + "\t" + fileStamp(fs::path(projectDir) / m) + "\n";
    for (const std::string& m : projectMaterials(projectDir)) {
        MaterialAsset asset;
        std::string error, line;
        if (readMaterialAsset(projectDir + "/" + m, asset, error)) {
            for (const std::string& t : asset.textures)
                if (!t.empty())
                    line += "\t" + t + "\t" + fileStamp(fs::path(projectDir) / t);
            if (!asset.height.empty())  // (the ORM pages' alpha: parallax; a displaced one's meshes too)
                line += "\theight\t" + fileStamp(fs::path(projectDir) / asset.height) + (asset.displaced() ? "\t" + materialCookKey(asset) : "");
        }
        if (!line.empty())
            stamp += m + line + "\n";
    }
    for (const auto& [mesh, material] : displacedMeshes(projectDir, relative))
        stamp += "displace\t" + mesh + "\t" + material + "\n";
    return stamp;
}

}  // namespace

std::vector<std::string> projectMaterials(const std::string& projectDir)
{
    std::vector<std::string> materials;
    for (const std::string& assets : libraries::assetDirs(projectDir)) {  // (Assets/, then its libraries')
        std::error_code ec;
        for (fs::recursive_directory_iterator it(fs::path(projectDir) / assets, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (ext == ".fmat" && it->is_regular_file(ec))
                materials.push_back(it->path().lexically_relative(projectDir).generic_string());
        }
    }
    std::sort(materials.begin(), materials.end());
    return materials;
}

std::vector<std::string> projectModels(const std::string& projectDir)
{
    std::vector<std::string> models;
    for (const std::string& dir : libraries::assetDirs(projectDir)) {  // (Assets/, then its libraries')
        std::error_code ec;
        const fs::path assets = fs::path(projectDir) / dir;
        for (fs::recursive_directory_iterator it(assets, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if ((ext == ".gltf" || ext == ".glb") && it->is_regular_file(ec))
                models.push_back(it->path().lexically_relative(projectDir).generic_string());
        }
    }
    std::sort(models.begin(), models.end());
    return models;
}

bool modelsChanged(const std::string& projectDir, const std::string& relative)
{
    std::string stamp;
    if (!readText(stampFile(projectDir, relative), stamp))  // never cooked with models: only if there are some
        return !projectModels(projectDir).empty() || !displacedMeshes(projectDir, relative).empty();
    return stamp != modelsStamp(projectDir, relative);
}

namespace {

// A scene's cook files: the lock, the cooker's progress and error output (Cache/Stamps/), and
// the cook itself before it is moved into place (Cache/Cook/).
struct CookFiles {
    std::string fpak, fvt, stamp;              // where the scene lives
    std::string stagedFpak, stagedFvt, stagedStamp;
    std::string lock, progress, errors;

    CookFiles(const Project& project, const std::string& relative)
    {
        const std::string name = cacheName(relative);
        fpak = project.path(relative);
        fvt = fs::path(fpak).replace_extension(".fvt").generic_string();
        stamp = stampFile(project.dir, relative);
        stagedFpak = project.dir + "/Cache/Cook/" + name;
        stagedFvt = fs::path(stagedFpak).replace_extension(".fvt").generic_string();
        stagedStamp = stagedFpak + ".models";
        lock = project.dir + "/Cache/Stamps/" + name + ".cooking";
        progress = project.dir + "/Cache/Stamps/" + name + ".progress";
        errors = project.dir + "/Cache/Stamps/" + name + ".errors";
    }
    bool installed(const std::string& stampText) const
    {
        std::error_code ec;
        std::string cooked;
        return fs::exists(fpak, ec) && readText(stamp, cooked) && cooked == stampText;
    }
    bool staged(const std::string& stampText) const
    {
        std::error_code ec;
        std::string cooked;
        return fs::exists(stagedFpak, ec) && fs::exists(stagedFvt, ec) && readText(stagedStamp, cooked) && cooked == stampText;
    }
};

CookProgress readCookProgress(const std::string& path)
{
    CookProgress p;
    std::string text;
    if (readText(path, text)) {
        std::istringstream in(text);
        in >> p.stage >> p.done >> p.total >> p.reused;
    }
    return p;
}

// Holds a scene's cook lock while it lives (the file names this process).
class CookLock {
public:
    explicit CookLock(std::string path) : path_(std::move(path)) {}
    ~CookLock()
    {
        std::error_code ec;
        if (held_)
            fs::remove(path_, ec);
    }
    // Waits while another process that is still running holds it (false: cancelled).
    bool take(const std::string& otherProgress, const CookProgressFn& progress, size_t models)
    {
        std::error_code ec;
        fs::create_directories(fs::path(path_).parent_path(), ec);
        bool reported = false;
        for (;;) {
            if (FILE* f = platform::openFile(fs::path(path_), "wx")) {  // created only if there is none
                std::fprintf(f, "%u\n", currentProcessId());
                std::fclose(f);
                held_ = true;
                return true;
            }
            std::string text;
            char* end = nullptr;
            const bool read = readText(path_, text) && !text.empty();
            const uint32_t pid = read ? uint32_t(std::strtoul(text.c_str(), &end, 10)) : 0;
            const auto age = fs::file_time_type::clock::now() - fs::last_write_time(path_, ec);
            if (ec || (read && !processRunning(pid)) || (!read && age > std::chrono::seconds(10))) {
                FX_INFO("project: %s was left by a process that is gone; ignored", path_.c_str());
                fs::remove(path_, ec);
                continue;
            }
            if (!reported) {
                FX_INFO("project: another process (%u) is cooking this scene; waiting for it", pid);
                reported = true;
            }
            if (progress) {
                CookProgress p = readCookProgress(otherProgress);
                p.stage = "waiting";
                p.models = models;
                if (!progress(p))
                    return false;
            }
            SDL_Delay(250);
        }
    }

private:
    std::string path_;
    bool held_ = false;
};

// The cooker's error output, its last lines, as one message ("falcon cook: " dropped).
std::string cookerErrors(const std::string& path, bool log)
{
    std::string text;
    if (!readText(path, text))
        return {};
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty())
            continue;
        if (log)
            FX_WARN("%s", line.c_str());
        if (line.starts_with("falcon cook: "))
            line = line.substr(13);
        lines.push_back(line);
    }
    std::string out;
    for (size_t i = lines.size() > 3 ? lines.size() - 3 : 0; i < lines.size(); ++i)
        out += (out.empty() ? "" : "\n") + lines[i];
    return out;
}

// The staged cook moved over the scene's files (retried while another window still has them
// open: the editor that cooked it, quitting).
bool installCook(const CookFiles& files, const std::string& relative, const CookProgressFn& progress, std::string& error)
{
    std::error_code ec;
    fs::create_directories(fs::path(files.stamp).parent_path(), ec);
    for (int attempt = 0;; ++attempt) {
        ec.clear();
        if (fs::exists(files.stagedFvt, ec))
            fs::rename(files.stagedFvt, files.fvt, ec);
        if (!ec)
            fs::rename(files.stagedFpak, files.fpak, ec);
        if (!ec)
            fs::rename(files.stagedStamp, files.stamp, ec);
        if (!ec)
            return true;
        if (progress)
            progress({"installing", 0, 0, 0, 0});
        if (attempt >= 120) {  // 30 s
            error = "Cannot replace " + relative + " (" + ec.message() + "). Is it open in another Falcon window?";
            return false;
        }
        SDL_Delay(250);
    }
}

}  // namespace

bool sceneNeedsCook(const Project& project, const std::string& relative)
{
    const CookFiles files(project, relative);
    const std::string stamp = modelsStamp(project.dir, relative);
    return !files.installed(stamp) && !files.staged(stamp);
}

bool cookProjectScene(const Project& project, const std::string& relative, std::string& error, const CookProgressFn& progress,
                      bool install)
{
    const CookFiles files(project, relative);
    std::string stamp = modelsStamp(project.dir, relative);
    if (files.installed(stamp))
        return true;
    const std::vector<std::string> models = projectModels(project.dir), materials = projectMaterials(project.dir);
    CookLock lock(files.lock);
    if (!lock.take(files.progress, progress, models.size())) {
        error = "Cancelled";
        return false;
    }
    stamp = modelsStamp(project.dir, relative);  // (another process may have cooked it meanwhile)
    if (files.installed(stamp))
        return true;
    if (files.staged(stamp))
        return !install || installCook(files, relative, progress, error);

    FX_INFO("project: cooking %s with %zu model%s and %zu material%s", relative.c_str(), models.size(), models.size() == 1 ? "" : "s",
            materials.size(), materials.size() == 1 ? "" : "s");
    std::error_code ec;
    fs::create_directories(fs::path(files.stagedFpak).parent_path(), ec);
    fs::remove(files.stagedStamp, ec);
    fs::remove(files.progress, ec);
    std::vector<std::string> args = {"cook", "--empty", files.stagedFpak, "--rocks", "--source-root", project.dir, "--progress", files.progress};
    for (const std::string& m : models) {
        args.push_back("--library");
        args.push_back(project.path(m));
    }
    for (const std::string& m : materials) {
        args.push_back("--material");
        args.push_back(project.path(m));
    }
    for (const auto& [mesh, material] : displacedMeshes(project.dir, relative)) {
        args.push_back("--displace");
        args.push_back(mesh + "=" + project.path(material));
    }
    // Texture pages encoded before: the scene's own (moved into Scenes/) and the other scenes'.
    std::vector<std::string> reuse = {files.fvt};
    for (const std::string& s : project.scenes)
        reuse.push_back(CookFiles(project, s).fvt);
    for (fs::directory_iterator it(fs::path(files.stagedFpak).parent_path(), ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".fvt" && it->path().generic_string() != files.stagedFvt)
            reuse.push_back(it->path().generic_string());
    std::sort(reuse.begin(), reuse.end());
    reuse.erase(std::unique(reuse.begin(), reuse.end()), reuse.end());
    for (const std::string& r : reuse)
        if (fs::exists(r, ec)) {
            args.push_back("--reuse");
            args.push_back(r);
        }

    void* child = startFalconTracked(args, files.errors);
    if (!child) {
        error = "Cannot start the cooker for " + relative;
        return false;
    }
    int code = -1;
    while (!falconFinished(child, code)) {
        CookProgress p = readCookProgress(files.progress);
        p.models = models.size();
        if (p.stage.empty())
            p.stage = "import";
        if (progress && !progress(p)) {
            stopFalcon(child);
            error = "Cancelled";
            return false;
        }
        SDL_Delay(100);
    }
    if (CookProgress p = readCookProgress(files.progress); progress && !p.stage.empty()) {  // its last line ("done")
        p.models = models.size();
        progress(p);
    }
    const std::string messages = cookerErrors(files.errors, true);
    if (code != 0) {
        error = "Cooking " + relative + " failed";
        error += messages.empty() ? " (the cooker exited with " + std::to_string(code) + ")." : ":\n" + messages;
        FX_ERROR("project: %s", error.c_str());
        return false;
    }
    if (!writeText(files.stagedStamp, stamp)) {
        error = "Cannot write " + files.stagedStamp;
        return false;
    }
    return !install || installCook(files, relative, progress, error);
}

bool createProject(const std::string& dir, const std::string& name, Project& out, std::string& error)
{
    std::error_code ec;
    const fs::path root(dir);
    if (fs::exists(root, ec) && !fs::is_empty(root, ec)) {
        error = "The folder is not empty: " + normalizedPath(root);
        return false;
    }
    for (const char* sub : {"Assets", "Scenes", "Cache"}) {
        fs::create_directories(root / sub, ec);
        if (ec) {
            error = "Cannot create " + normalizedPath(root / sub) + ": " + ec.message();
            return false;
        }
    }
    out = {};
    out.dir = normalizedPath(root);
    out.name = name.empty() ? root.filename().string() : name;
    out.startupScene = "Scenes/Main.fpak";
    out.scenes = {out.startupScene};
    if (!cookProjectScene(out, out.startupScene, error) || !saveProject(out, error) ||
        !gamescripts::create(out.dir, out.name, error))
        return false;
    FX_INFO("project: created %s (%s)", out.name.c_str(), out.dir.c_str());
    return true;
}

bool addScene(Project& project, const std::string& name, const std::string& folder, std::string& relative, std::string& error,
              bool cook)
{
    const std::string safe = fileSafeName(name);
    if (safe.empty()) {
        error = "A scene needs a name";
        return false;
    }
    const std::string dir = fs::path(folder).lexically_normal().generic_string();
    if (dir.starts_with("..") || fs::path(dir).is_absolute()) {
        error = "Scenes go inside the project";
        return false;
    }
    relative = (dir.empty() || dir == "." ? "" : dir + "/") + safe + ".fpak";
    std::error_code ec;
    if (std::find(project.scenes.begin(), project.scenes.end(), relative) != project.scenes.end() ||
        fs::exists(project.path(relative), ec)) {
        error = "There is a scene called " + safe + " already";
        return false;
    }
    if (cook && !cookProjectScene(project, relative, error))
        return false;
    project.scenes.push_back(relative);
    return saveProject(project, error);
}

RecentProjects::RecentProjects(bool persistent)
{
    if (!persistent)
        return;
    if (char* dir = SDL_GetPrefPath("Falcon", "Editor")) {
        path_ = std::string(dir) + "projects.json";
        SDL_free(dir);
    }
    std::string text;
    if (path_.empty() || !readText(path_, text))
        return;
    try {
        for (const Json& j : Json::parse(text).array)
            list_.push_back({j.getString("dir"), j.getString("name"), int64_t(j.getNumber("opened"))});
    } catch (const std::exception& e) {
        FX_WARN("project: %s is malformed (%s)", path_.c_str(), e.what());
        list_.clear();
    }
}

void RecentProjects::touch(const Project& project)
{
    forget(project.dir);
    list_.insert(list_.begin(), {project.dir, project.name, int64_t(std::time(nullptr))});
    if (list_.size() > kMaxRecent)
        list_.resize(kMaxRecent);
    save();
}

void RecentProjects::forget(const std::string& dir)
{
    const std::string d = normalizedPath(dir);
    list_.erase(std::remove_if(list_.begin(), list_.end(), [&](const RecentProject& r) { return r.dir == d; }), list_.end());
    save();
}

bool RecentProjects::contains(const std::string& dir) const
{
    const std::string d = normalizedPath(dir);
    return std::any_of(list_.begin(), list_.end(), [&](const RecentProject& r) { return r.dir == d; });
}

void RecentProjects::save() const
{
    if (path_.empty())
        return;
    JsonWriter w;
    w.beginArray();
    for (const RecentProject& r : list_)
        w.beginObject().value("dir", r.dir).value("name", r.name).value("opened", r.opened).endObject();
    w.endArray();
    if (!writeText(path_, w.str()))
        FX_WARN("project: cannot write %s", path_.c_str());
}

}  // namespace fx
