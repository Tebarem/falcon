#include "editor/prefab.h"

#include "core/json.h"
#include "core/log.h"
#include "editor/scene_edits.h"
#include "level/poly_mesh.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace fx {

namespace {

void multiply(const float a[3][4], const float b[3][4], float out[3][4])
{
    float r[3][4];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j)
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
        r[i][3] += a[i][3];
    }
    std::memcpy(out, r, sizeof(r));
}

}  // namespace

const PrefabEntity* Prefab::find(const std::string& id) const
{
    for (const PrefabEntity& e : entities)
        if (e.id == id)
            return &e;
    return nullptr;
}

std::vector<std::array<float, 12>> Prefab::rootRelative() const
{
    std::vector<std::array<float, 12>> out(entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
        float m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        if (i > 0) {
            std::memcpy(m, entities[i].local, sizeof(m));
            for (size_t p = 0; p < i; ++p)  // parents come first
                if (entities[p].id == entities[i].parent) {
                    float parent[3][4];
                    std::memcpy(parent, out[p].data(), sizeof(parent));
                    multiply(parent, entities[i].local, m);
                    break;
                }
        }
        std::memcpy(out[i].data(), m, sizeof(m));
    }
    return out;
}

bool readPrefabFile(const std::string& path, Prefab& out, std::string& error)
{
    size_t size = 0;
    void* text = SDL_LoadFile(path.c_str(), &size);
    if (!text) {
        error = "Cannot read " + path;
        return false;
    }
    try {
        const Json j = Json::parse(std::string_view(static_cast<const char*>(text), size));
        SDL_free(text);
        out = {};
        out.name = j.getString("name", std::filesystem::path(path).stem().string().c_str());
        if (const Json* list = j.get("entities"))
            for (const Json& e : list->array) {
                PrefabEntity p;
                p.id = e.getString("id");
                p.parent = e.getString("parent");
                p.name = e.getString("name");
                p.kind = e.getString("kind", "empty");
                p.mesh = e.getString("mesh");
                p.material = e.getString("material");
                p.hidden = e.getBool("hidden");
                if (const Json* l = e.get("local"); l && l->array.size() == 12)
                    for (int k = 0; k < 12; ++k)
                        p.local[k / 4][k % 4] = float(l->array[size_t(k)].number);
                if (const Json* l = e.get("light"); l && l->isObject()) {
                    p.light = Light{};
                    readLightJson(*l, *p.light);
                }
                if (const Json* c = e.get("camera"); c && c->isObject())
                    p.camera = CameraComponent{float(c->getNumber("fov", 60.0)), float(c->getNumber("near", 0.05)), c->getBool("main") ? 1u : 0u};
                if (const Json* b = e.get("body"); b && b->isObject()) {
                    p.body = PhysicsBody{};
                    readBodyJson(*b, *p.body);
                }
                if (const Json* c = e.get("collider"); c && c->isObject()) {
                    p.collider = Collider{};
                    readColliderJson(*c, *p.collider);
                }
                if (const Json* a = e.get("audioSource"); a && a->isObject()) {
                    p.audioSource = AudioSource{};
                    readAudioSourceJson(*a, *p.audioSource);
                }
                if (const Json* l = e.get("audioListener"); l && l->isObject()) {
                    p.audioListener = AudioListener{};
                    readAudioListenerJson(*l, *p.audioListener);
                }
                if (const Json* a = e.get("acoustics"); a && a->isObject()) {
                    p.acoustics = AcousticSurface{};
                    readAcousticSurfaceJson(*a, *p.acoustics);
                }
                readVfxJson(e, p.vfx);
                readScriptsJson(e, p.scripts);
                if (const Json* m = e.get("levelMesh"); m && !m->str.empty()) {
                    auto mesh = std::make_shared<level::PolyMesh>();
                    if (mesh->loadText(m->str))
                        p.levelMesh = std::move(mesh);
                }
                readLevelParts(e, p.level);
                p.prefab = e.getString("prefab");
                if (const Json* o = e.get("overrides"); o && o->isObject())
                    readOverrides(*o, p.overrides);
                out.entities.push_back(std::move(p));
            }
    } catch (const std::exception& e) {
        error = path + " is malformed (" + e.what() + ")";
        return false;
    }
    if (out.entities.empty() || !out.entities[0].parent.empty()) {
        error = path + " has no root entity";
        return false;
    }
    return true;
}

bool writePrefab(const std::string& path, const Prefab& prefab, std::string& error)
{
    JsonWriter w;
    w.beginObject().value("format", 1u).value("name", prefab.name).beginArray("entities");
    for (const PrefabEntity& e : prefab.entities) {
        w.beginObject().value("id", e.id);
        if (!e.parent.empty())
            w.value("parent", e.parent);
        w.value("name", e.name).value("kind", e.kind);
        if (!e.mesh.empty())
            w.value("mesh", e.mesh);
        if (!e.material.empty())
            w.value("material", e.material);
        if (!e.parent.empty()) {
            w.beginArray("local");
            for (int k = 0; k < 12; ++k)
                w.value(nullptr, double(e.local[k / 4][k % 4]));
            w.endArray();
        }
        if (e.hidden)
            w.value("hidden", true);
        if (e.light)
            writeLightJson(w, *e.light);
        if (e.camera)
            w.beginObject("camera").value("fov", double(e.camera->fov)).value("near", double(e.camera->nearClip))
                .value("main", (e.camera->flags & 1u) != 0).endObject();
        if (e.body)
            writeBodyJson(w, *e.body);
        if (e.collider)
            writeColliderJson(w, *e.collider);
        if (e.audioSource)
            writeAudioSourceJson(w, *e.audioSource);
        if (e.audioListener)
            writeAudioListenerJson(w, *e.audioListener);
        if (e.acoustics)
            writeAcousticSurfaceJson(w, *e.acoustics);
        writeVfxJson(w, e.vfx);
        writeScriptsJson(w, e.scripts);
        if (e.levelMesh)
            w.value("levelMesh", e.levelMesh->saveText());
        writeLevelParts(w, e.level);
        if (!e.prefab.empty()) {
            w.value("prefab", e.prefab);
            if (!e.overrides.empty())
                writeOverrides(w, e.overrides);
        }
        w.endObject();
    }
    w.endArray().endObject();
    const std::string json = Json::parse(w.str()).dump(2) + "\n";
    const std::string tmp = path + ".tmp";
    std::error_code ec;
    const auto u8 = [](const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); };
    if (!SDL_SaveFile(tmp.c_str(), json.data(), json.size())) {
        error = "Cannot write " + path;
        return false;
    }
    std::filesystem::rename(u8(tmp), u8(path), ec);
    if (ec) {
        error = "Cannot write " + path + " (" + ec.message() + ")";
        return false;
    }
    return true;
}

void applyOverride(PrefabEntity& e, const PrefabOverride& o)
{
    if (o.hasLocal)
        std::memcpy(e.local, o.local, sizeof(e.local));
    if (o.hidden)
        e.hidden = *o.hidden;
    if (o.material)
        e.material = *o.material;
    if (o.name)
        e.name = *o.name;
    if (o.light)
        e.light = o.light;
    if (o.camera)
        e.camera = o.camera;
    if (o.body)
        e.body = o.body;
    if (o.collider)
        e.collider = o.collider;
    if (o.audioSource)
        e.audioSource = o.audioSource;
    if (o.audioListener)
        e.audioListener = o.audioListener;
    if (o.acoustics)
        e.acoustics = o.acoustics;
    if (o.vfx)
        e.vfx = *o.vfx;
    if (o.scripts)
        e.scripts = *o.scripts;
}

namespace {

// The project a prefab is in: the nearest folder above it with project.falcon.
std::string projectOf(const std::string& path)
{
    std::error_code ec;
    for (std::filesystem::path dir = std::filesystem::path(path).parent_path(); !dir.empty(); dir = dir.parent_path()) {
        if (std::filesystem::exists(dir / "project.falcon", ec))
            return dir.generic_string();
        if (dir == dir.parent_path())
            break;
    }
    return {};
}

bool flatten(const std::string& projectDir, const std::string& path, Prefab& out, std::string& error, std::vector<std::string>& stack)
{
    Prefab file;
    if (!readPrefabFile(path, file, error))
        return false;
    const std::string key = std::filesystem::path(path).lexically_normal().generic_string();
    stack.push_back(key);
    out = {};
    out.name = file.name;
    for (PrefabEntity& e : file.entities) {
        if (e.kind != "prefab") {
            out.entities.push_back(std::move(e));
            continue;
        }
        // A nested instance: its prefab's entities in its place, under path ids.
        Prefab inner;
        std::string innerError;
        const std::string innerPath = projectDir.empty() ? e.prefab : projectDir + "/" + e.prefab;
        const std::string innerKey = std::filesystem::path(innerPath).lexically_normal().generic_string();
        const bool cycle = std::find(stack.begin(), stack.end(), innerKey) != stack.end();
        if (cycle || stack.size() >= 8 || !flatten(projectDir, innerPath, inner, innerError, stack)) {
            FX_WARN("prefab %s: nested %s %s (an empty stands in)", path.c_str(), e.prefab.c_str(),
                    cycle ? "contains this prefab" : stack.size() >= 8 ? "nests too deep" : innerError.c_str());
            e.kind = "empty";
            e.prefab.clear();
            e.overrides.clear();
            out.entities.push_back(std::move(e));
            continue;
        }
        const std::string innerRoot = inner.entities[0].id;  // (before it is moved out below)
        std::vector<std::string> removed;  // (removed in this nesting, and what hangs under them)
        for (size_t k = 0; k < inner.entities.size(); ++k) {
            PrefabEntity x = std::move(inner.entities[k]);
            const std::string innerId = x.id;
            bool gone = k > 0 && std::find(removed.begin(), removed.end(), x.parent) != removed.end();
            for (const PrefabOverride& o : e.overrides)
                if (o.id == innerId) {
                    applyOverride(x, o);
                    gone |= k > 0 && o.removed;
                }
            if (gone) {
                removed.push_back(innerId);
                continue;
            }
            x.nestedIn = e.prefab;
            x.nestedId = innerId;
            if (k == 0) {  // the nested root: the entity's own place, name and visibility
                x.id = e.id;
                x.parent = e.parent;
                x.name = e.name.empty() ? x.name : e.name;
                std::memcpy(x.local, e.local, sizeof(x.local));
                x.hidden = e.hidden;
            } else {
                x.id = e.id + "/" + innerId;
                x.parent = x.parent == innerRoot ? e.id : e.id + "/" + x.parent;
            }
            out.entities.push_back(std::move(x));
        }
    }
    stack.pop_back();
    return true;
}

}  // namespace

bool readPrefab(const std::string& path, Prefab& out, std::string& error, const std::string& projectDir)
{
    std::vector<std::string> stack;
    if (!flatten(projectDir.empty() ? projectOf(path) : projectDir, path, out, error, stack))
        return false;
    if (out.entities.empty() || !out.entities[0].parent.empty()) {
        error = path + " has no root entity";
        return false;
    }
    return true;
}

}  // namespace fx
