#include "editor/launcher.h"

#include "app/process.h"
#include "core/log.h"
#include "ui/ui_layer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <filesystem>

namespace fx {

namespace fs = std::filesystem;

Launcher::~Launcher() { releaseFalcon(opening_); }

void Launcher::init(const Init& init)
{
    ui_ = init.ui;
    window_ = init.window;
    relaunch_ = init.relaunch;
    recent_ = std::make_unique<RecentProjects>(init.persistent);
    if (const char* docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS))
        defaultLocation_ = (fs::path(docs) / "Falcon Projects").lexically_normal().generic_string();
}

bool Launcher::loadScript(const char* path)
{
    size_t size = 0;
    void* text = SDL_LoadFile(path, &size);
    if (!text) {
        FX_ERROR("launcher: cannot read script %s", path);
        return false;
    }
    try {
        const Json list = Json::parse(std::string_view(static_cast<const char*>(text), size));
        SDL_free(text);
        for (const Json& step : list.array)
            script_.push_back(step.dump(0));
    } catch (const std::exception& e) {
        FX_ERROR("launcher: script %s: %s", path, e.what());
        return false;
    }
    return true;
}

void Launcher::fail(const std::string& message)
{
    error_ = message;
    FX_WARN("launcher: %s", message.c_str());
    sendState();
}

void Launcher::sendState()
{
    JsonWriter w;
    w.beginObject().value("type", "launcher.state").beginArray("recent");
    for (const RecentProject& r : recent_->list()) {
        std::error_code ec;
        w.beginObject()
            .value("dir", r.dir)
            .value("name", r.name)
            .value("opened", r.opened)
            .value("exists", fs::exists(fs::path(r.dir) / "project.falcon", ec))
            .endObject();
    }
    w.endArray()
        .value("defaultLocation", defaultLocation_)
        .value("busy", busy())
        .value("busyText", cooking_.valid() || opening_ ? busyText_ : std::string("Creating the project…"))
        .value("error", error_)
        .value("version", "0.1")
        .endObject();
    ui_->send(w.str());
}

void Launcher::open(const std::string& path)
{
    Project p;
    std::string error;
    if (!loadProject(path, p, error))
        return fail(error);
    recent_->touch(p);
    if (busy())
        return;
    if (sceneNeedsCook(p, p.startupScene)) {
        FX_INFO("launcher: cooking %s of %s before opening it", p.startupScene.c_str(), p.dir.c_str());
        cookingProject_ = p;
        busyText_ = "Cooking " + p.startupScene + "…";
        cookProgress_ = {};
        cooking_ = std::async(std::launch::async, [this, p] {
            std::string error;
            const bool ok = cookProjectScene(p, p.startupScene, error, [this](const CookProgress& progress) {
                std::lock_guard<std::mutex> lock(cookMutex_);
                cookProgress_ = progress;
                return true;
            });
            return std::make_pair(ok, error);
        });
        sendState();
        return;
    }
    opened(p);
}

void Launcher::opened(const Project& p)
{
    if (!relaunch_) {
        FX_INFO("launcher: would open %s (--no-relaunch)", p.dir.c_str());
        opened_ = p.dir;
        ++opens_;
        sendState();
        return;
    }
    FX_INFO("launcher: opening %s", p.dir.c_str());
    if (!(opening_ = startFalconTracked({"--editor", "--project", p.dir})))
        return fail("Could not start the editor");
    openingNs_ = SDL_GetTicksNS();
    busyText_ = "Opening " + p.name + "…";
    sendState();
}

void Launcher::handle(const Json& m)
{
    const std::string type = m.getString("type");
    if (type == "launcher.ready") {
        if (!connected_) {
            connected_ = true;
            connectedNs_ = SDL_GetTicksNS();
        }
        sendState();
    } else if (type == "launcher.browse") {
        // Answers on SDL's thread (queued for the main thread in update()).
        struct Request {
            Launcher* self;
            std::string purpose;
        };
        auto* request = new Request{this, m.getString("purpose", "open")};
        std::error_code ec;
        const std::string start = fs::exists(defaultLocation_, ec) ? defaultLocation_ : std::string();
        SDL_ShowOpenFolderDialog(
            [](void* user, const char* const* files, int) {
                auto* r = static_cast<Request*>(user);
                if (files && files[0]) {
                    std::lock_guard<std::mutex> lock(r->self->pickedMutex_);
                    r->self->picked_.push_back({r->purpose, fs::path(files[0]).lexically_normal().generic_string()});
                }
                delete r;
            },
            request, window_, start.empty() ? nullptr : start.c_str(), false);
    } else if (type == "launcher.create") {
        if (busy())
            return;
        const std::string name = m.getString("name");
        const std::string folder = fileSafeName(name);
        const std::string location = m.getString("location", defaultLocation_.c_str());
        if (folder.empty())
            return fail("A project needs a name (letters, digits, spaces, - and _)");
        if (location.empty())
            return fail("Pick a folder to put the project in");
        const std::string dir = (fs::path(location) / folder).lexically_normal().generic_string();
        std::error_code ec;
        if (fs::exists(dir, ec) && !fs::is_empty(dir, ec))
            return fail("There is something in " + dir + " already");
        error_.clear();
        creating_ = std::async(std::launch::async, [this, dir, name] {
            std::string error;
            const bool ok = createProject(dir, name, created_, error);
            return std::make_pair(ok, error);
        });
        sendState();
    } else if (type == "launcher.open") {
        error_.clear();
        open(m.getString("path"));
    } else if (type == "launcher.forget") {
        recent_->forget(m.getString("path"));
        sendState();
    } else if (type == "test.mouse" || type == "test.probe") {
        // Real input: the page is asked where "at" is (launcher.probe), then SDL events go
        // through the UI layer and cef-lite to it, aimed at (fx, fy) of the rect. test.probe
        // only asks (test.expect value / focused / dialog check the answer).
        if (type == "test.mouse")
            mouse_ = m.dump(0);
        JsonWriter w;
        w.beginObject().value("type", "launcher.probe").value("at", m.getString("at")).endObject();
        ui_->send(w.str());
    } else if (type == "launcher.probed") {
        probed_ = m.dump(0);
        if (!mouse_.empty())
            click(Json::parse(std::exchange(mouse_, std::string())), m);
    } else if (type == "test.type") {
        // As SDL delivers typing once text input is on: text events (the page's field gets
        // the characters). The text lives until the next test.type (SDL keeps the pointer).
        typed_ = m.getString("text");
        SDL_Event e = {};
        e.type = SDL_EVENT_TEXT_INPUT;
        e.text.windowID = SDL_GetWindowID(window_);
        e.text.text = typed_.c_str();
        SDL_PushEvent(&e);
    } else if (type == "test.key") {
        SDL_Event e = {};
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.windowID = SDL_GetWindowID(window_);
        e.key.key = SDL_GetKeyFromName(m.getString("key").c_str());
        e.key.scancode = SDL_GetScancodeFromKey(e.key.key, nullptr);
        e.key.down = true;
        SDL_PushEvent(&e);
        e.type = SDL_EVENT_KEY_UP;
        e.key.down = false;
        SDL_PushEvent(&e);
    } else if (type == "test.wait") {
        scriptWait_ = uint32_t(m.getNumber("frames", 0));
        scriptWaitUntilNs_ = SDL_GetTicksNS() + uint64_t(m.getNumber("ms", 0) * 1e6);
    } else if (type == "test.capture") {
        capture_ = m.getString("path");
    } else if (type == "test.exit") {
        exit_ = true;
    } else if (type == "test.expect") {
        std::string why;
        std::error_code ec;
        if (const Json* v = m.get("projectExists"); v && !fs::exists(fs::path(v->str) / "project.falcon", ec))
            why += " project " + v->str;
        if (const Json* v = m.get("sceneExists"); v && !fs::exists(v->str, ec))
            why += " scene " + v->str;
        if (const Json* v = m.get("recentContains"); v && !recent_->contains(v->str))
            why += " recent " + v->str;
        if (const Json* v = m.get("opened"); v && absolutePath(v->str) != opened_)
            why += " opened '" + opened_ + "'";
        if (const Json* v = m.get("opens"); v && uint32_t(v->number) != opens_)
            why += " opens " + std::to_string(opens_);
        if (const Json* v = m.get("textInput"); v && v->b != SDL_TextInputActive(window_))
            why += v->b ? " text input off" : " text input on";
        if (m.get("value") || m.get("focused") || m.get("dialog")) {
            const Json p = probed_.empty() ? Json() : Json::parse(probed_);
            if (const Json* v = m.get("value"); v && p.getString("value") != v->str)
                why += " value '" + p.getString("value") + "'";
            if (const Json* v = m.get("focused"); v && p.getBool("focused") != v->b)
                why += " focused " + std::string(p.getBool("focused") ? "true" : "false");
            if (const Json* v = m.get("dialog"); v && p.getBool("dialog") != v->b)
                why += " dialog " + std::string(p.getBool("dialog") ? "open" : "closed");
        }
        if (const Json* v = m.get("error"); v && v->b != !error_.empty())
            why += " error '" + error_ + "'";
        if (!why.empty()) {
            FX_ERROR("launcher test: expectation failed:%s", why.c_str());
            testFailed_ = true;
        }
    }
}

void Launcher::click(const Json& step, const Json& probed)
{
    const Json* r = probed.get("rect");
    if (!r || !r->isObject()) {
        FX_ERROR("launcher test: nothing on the page matches '%s'", step.getString("at").c_str());
        testFailed_ = true;
        return;
    }
    float x = 0.0f, y = 0.0f;
    ui_->pageToWindow(float(r->getNumber("x") + r->getNumber("width") * step.getNumber("fx", 0.5)),
                      float(r->getNumber("y") + r->getNumber("height") * step.getNumber("fy", 0.5)), x, y);
    SDL_Event e = {};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = SDL_GetWindowID(window_);
    e.motion.x = x;
    e.motion.y = y;
    SDL_PushEvent(&e);
    if (step.getString("action", "click") == "move")
        return;
    e = {};
    e.button.windowID = SDL_GetWindowID(window_);
    e.button.button = SDL_BUTTON_LEFT;
    e.button.clicks = 1;
    e.button.x = x;
    e.button.y = y;
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.down = true;
    SDL_PushEvent(&e);
    e.type = SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.down = false;
    SDL_PushEvent(&e);
}

void Launcher::update()
{
    if (!ui_ || !ui_->active())
        return;
    std::string message;
    while (ui_->poll(message)) {
        try {
            handle(Json::parse(message));
        } catch (const std::exception& e) {
            FX_WARN("launcher: bad message (%s)", e.what());
        }
    }
    {
        std::lock_guard<std::mutex> lock(pickedMutex_);
        for (const auto& [purpose, path] : picked_) {
            JsonWriter w;
            w.beginObject().value("type", "launcher.picked").value("purpose", purpose).value("path", path).endObject();
            ui_->send(w.str());
            if (purpose == "open")
                open(path);
        }
        picked_.clear();
    }
    if (creating_.valid() && creating_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const auto [ok, error] = creating_.get();
        if (ok)
            open(created_.dir);
        else
            fail(error);
    }
    if (cooking_.valid()) {
        CookProgress p;
        {
            std::lock_guard<std::mutex> lock(cookMutex_);
            p = cookProgress_;
        }
        // "Cooking Main: 140 models · encoding textures 37%"
        std::string scene = fs::path(cookingProject_.startupScene).stem().string(), text = "Cooking " + scene;
        if (p.models)
            text += ": " + std::to_string(p.models) + " model" + (p.models == 1 ? "" : "s");
        if (p.stage == "waiting") {
            text += " · another window is cooking it";
        } else if (!p.stage.empty()) {
            static const std::pair<const char*, std::array<int, 2>> kStages[] = {
                {"import", {0, 5}}, {"meshes", {5, 10}}, {"textures", {10, 95}}, {"writing", {95, 100}}, {"installing", {100, 100}}, {"done", {100, 100}}};
            for (const auto& [name, range] : kStages)
                if (p.stage == name) {
                    const float part = p.total ? std::min(1.0f, float(p.done) / float(p.total)) : 0.0f;
                    text += " · " + std::string(p.stage == "textures" ? "encoding textures" : p.stage == "meshes" ? "building meshes"
                                                                       : p.stage == "import" ? "reading the models" : "writing") +
                            " " + std::to_string(int(range[0] + (range[1] - range[0]) * part)) + "%";
                }
        }
        if (text + "…" != busyText_) {
            busyText_ = text + "…";
            sendState();
        }
        if (cooking_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const auto [ok, error] = cooking_.get();
            if (ok)
                opened(cookingProject_);
            else
                fail(error);
            sendState();
        }
    }
    // The editor's window up (or a while gone by: it may be cooking first): the launcher goes.
    if (opening_) {
        int code = 0;
        if (falconFinished(opening_, code)) {
            opening_ = nullptr;
            if (code != 0)
                fail("The editor stopped (exit code " + std::to_string(code) + ")");
            else
                exit_ = true;
            sendState();
        } else if (processShowsWindow(opening_) || SDL_GetTicksNS() - openingNs_ > 30'000'000'000ull) {
            exit_ = true;
        }
    }
    // Test script: a step per frame once the page is up (and between waits).
    if (scriptWait_ > 0 || SDL_GetTicksNS() < scriptWaitUntilNs_) {
        if (scriptWait_ > 0)
            --scriptWait_;
    } else if (connected_ && !busy() && SDL_GetTicksNS() - connectedNs_ > 500'000'000ull && scriptNext_ < script_.size()) {
        FX_INFO("launcher: script step %zu: %s", scriptNext_, script_[scriptNext_].c_str());
        try {
            handle(Json::parse(script_[scriptNext_++]));
        } catch (const std::exception& e) {
            FX_ERROR("launcher: script step: %s", e.what());
            testFailed_ = true;
        }
    }
}

}  // namespace fx
