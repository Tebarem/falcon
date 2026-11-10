#include "editor/anim_editor.h"

#include "anim/anim_events.h"
#include "anim/animation_system.h"
#include "anim/graph_library.h"
#include "assets/scene_pak.h"
#include "core/json.h"
#include "editor/anim_preview.h"
#include "ui/ui_layer.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <fstream>
#include <sstream>
#include <vector>

namespace fx {

namespace {

// The skeleton of a model's first skinned mesh (-1: none).
int32_t modelSkeleton(const ScenePak& pak, const ScenePak::Library& lib)
{
    uint32_t count = 0;
    const auto* skins = pak.view<fpak::MeshSkinGpu>(fpak::Section::MeshSkins, &count);
    for (const ScenePak::LibraryNode& n : lib.nodes)
        if (n.mesh < count && skins[n.mesh].firstSkinVertex != fpak::kNotSkinned)
            return int32_t(skins[n.mesh].skeleton);
    return -1;
}

}  // namespace

// A project path's file ("" when outside the project, or no project).
std::filesystem::path projectFile(const std::string& dir, const std::string& path)
{
    if (dir.empty() || path.empty() || path.find("..") != std::string::npos || std::filesystem::path(path).is_absolute())
        return {};
    return std::filesystem::path(dir) / path;
}

void AnimEditor::init(AnimPreview* preview, GraphLibrary* graphs, AnimationSystem* animation, const ScenePak* pak, std::string projectDir)
{
    preview_ = preview;
    graphs_ = graphs;
    animation_ = animation;
    pak_ = pak;
    projectDir_ = std::move(projectDir);
}

bool AnimEditor::handle(const std::string& type, const Json& m, UiLayer& ui)
{
    if (!type.starts_with("editor.anim."))
        return false;
    if (type == "editor.anim.open") {  // (scripts, the engine's side: the page opens it as from the Project panel)
        JsonWriter w;
        w.beginObject().value("type", "editor.anim.show").value("path", m.getString("path")).endObject();
        ui.send(w.str());
        return true;
    }
    if (type == "editor.anim.do") {  // (scripts) an edit the canvas makes, by name: the page runs it
        ui.send(m.dump(0));
        return true;
    }
    if (!preview_ || !graphs_) {
        if (type == "editor.anim.graph.open")
            sendPreview(ui, "The preview needs a scene with skinned meshes");
        return true;
    }
    if (type == "editor.anim.graph.open") {
        graph_ = m.getString("path");
        const auto def = graphs_->get(graph_);
        // The preview: the model it shows when that one fits the graph's skeleton, else the first that does.
        std::string error;
        if (def && (!preview_->active() || preview_->skeleton() != def->skeleton)) {
            error = "No model in this scene has the graph's skeleton";
            for (const ScenePak::Library& lib : pak_->libraries())
                if (modelSkeleton(*pak_, lib) == int32_t(def->skeleton) && preview_->open(lib.source, error)) {
                    error.clear();
                    break;
                }
        }
        if (preview_->active())
            preview_->setGraph(def);
        sendGraph(ui, true);
        sendPreview(ui, error);
    } else if (type == "editor.anim.preview.open") {
        std::string error;
        if (preview_->open(m.getString("model"), error))
            preview_->setGraph(graphs_->get(graph_));
        sendPreview(ui, error);
    } else if (type == "editor.anim.preview.set") {
        AnimPreview::Options& o = preview_->options;
        o.bones = m.getBool("bones", o.bones);
        o.grid = m.getBool("grid", o.grid);
        o.shadows = m.getBool("shadows", o.shadows);
        o.moveRoot = m.getBool("moveRoot", o.moveRoot);
        o.paused = m.getBool("paused", o.paused);
        o.speed = std::clamp(float(m.getNumber("speed", o.speed)), 0.0f, 10.0f);
        sendPreview(ui, "");
    } else if (type == "editor.anim.preview.restart") {
        preview_->restart();
    } else if (type == "editor.anim.preview.step") {
        preview_->step(float(m.getNumber("seconds", 1.0 / 30.0)));
    } else if (type == "editor.anim.preview.frame") {
        preview_->frame();
    } else if (type == "editor.anim.param") {
        if (anim::GraphInstance* g = preview_->graph()) {
            const std::string name = m.getString("name");
            const int32_t i = g->parameterIndex(name.c_str());
            const Json* v = m.get("value");
            if (i >= 0 && v && v->type == Json::Type::Bool)
                g->setBool(i, v->b);
            else if (i >= 0 && v && v->type == Json::Type::Number) {
                if (g->parameterType(uint32_t(i)) == anim::fmt::NodeType::ParamBool)
                    g->setBool(i, v->number != 0.0);
                else
                    g->setFloat(i, float(v->number));
            } else if (i >= 0 && v && v->type == Json::Type::String)
                g->setId(i, anim::fmt::hashId(v->str.c_str()));
        }
    } else if (type == "editor.anim.graph.compile") {
        const std::string path = m.getString("path");
        const auto old = graphs_->get(path);
        const auto compiled = graphs_->compile(path, m.getString("text"));
        if (compiled->ok())
            swapTo(path, old);
        if (path == graph_)
            sendGraph(ui);
    } else if (type == "editor.anim.graph.save") {
        const std::string path = m.getString("path");
        const auto old = graphs_->get(path);
        const std::filesystem::path file = projectFile(projectDir_, path);
        std::ofstream out(file, std::ios::binary);
        if (file.empty() || !out) {
            JsonWriter w;
            w.beginObject().value("type", "editor.anim.graph").value("path", path).value("saveError", "Can't write " + path).endObject();
            ui.send(w.str());
            return true;
        }
        const std::string text = m.getString("text");
        out.write(text.data(), std::streamsize(text.size()));
        out.close();
        swapTo(path, old);  // (compiled from the file now: what the characters play)
        if (path == graph_)
            sendGraph(ui, false, true);
    } else if (type == "editor.anim.graph.create") {
        // A state machine with one state playing the model's first clip, for its skeleton.
        const std::string folder = m.getString("folder", "Assets");
        std::string model = m.getString("model");
        for (const ScenePak::Library& lib : pak_->libraries())  // (none given: the first skinned model)
            if (model.empty() && modelSkeleton(*pak_, lib) >= 0)
                model = lib.source;
        std::string clip;
        const anim::AnimData* data = graphs_->data();
        for (const ScenePak::Library& lib : pak_->libraries())
            if (lib.source == model)
                if (const int32_t s = modelSkeleton(*pak_, lib); s >= 0 && data)
                    for (uint32_t c = 0; c < data->clipCount() && clip.empty(); ++c)
                        if (data->clip(c).skeleton == uint32_t(s))
                            clip = data->name(data->clip(c).name);
        const std::string stem = model.empty() ? "New Graph" : std::filesystem::path(model).stem().string();
        std::string path;
        for (int n = 0; n < 1000 && path.empty(); ++n) {
            const std::string candidate = folder + "/" + stem + (n ? " " + std::to_string(n) : std::string()) + ".fgraph";
            std::error_code ec;
            const std::filesystem::path f = projectFile(projectDir_, candidate);
            if (!f.empty() && !std::filesystem::exists(f, ec))
                path = candidate;
        }
        std::filesystem::path file = projectFile(projectDir_, path);
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream out(file, std::ios::binary);
        if (path.empty() || !out)
            return true;
        JsonWriter w;
        w.beginObject().value("version", 2.0).value("name", stem).value("skeleton", model);
        w.beginObject("parameters").beginObject("Speed").value("type", "float").value("default", 0.0).endObject().endObject();
        w.value("root", "@machine").beginObject("nodes");
        w.beginObject("machine").value("type", "stateMachine").value("default", "Idle").beginObject("states");
        w.beginObject("Idle").value("pose", "@idle").beginObject("editor").beginArray("pos").value(nullptr, 0.0).value(nullptr, 0.0).endArray().endObject().endObject();
        w.endObject().beginObject("editor").beginArray("pos").value(nullptr, 0.0).value(nullptr, 0.0).endArray().endObject().endObject();
        w.beginObject("idle").value("type", "clip").value("clip", clip).beginObject("editor").value("graph", "machine:Idle")
            .beginArray("pos").value(nullptr, -260.0).value(nullptr, 0.0).endArray().endObject().endObject();
        w.endObject().endObject();
        const std::string text = Json::parse(w.str()).dump(2);
        out.write(text.data(), std::streamsize(text.size()));
        out.close();
        created_ = true;
        JsonWriter show;
        show.beginObject().value("type", "editor.anim.show").value("path", path).endObject();
        ui.send(show.str());
    } else if (type == "editor.anim.events.read") {
        sendEvents(ui, m.getString("model"), "");
    } else if (type == "editor.anim.events.save") {
        // A model's clip markup: written beside it, onto its clips at once (graphs start again).
        const std::string model = m.getString("model"), text = m.getString("text");
        anim::Markup markup;
        std::string error;
        const int32_t skeleton = animation_ ? animation_->modelSkeleton(model) : -1;
        const std::filesystem::path file = projectFile(projectDir_, anim::markupPathFor(model));
        if (skeleton < 0)
            error = model + " has no skinned mesh in this scene";
        else if (!anim::parseMarkup(text, markup, error))
            ;
        else if (std::ofstream out{file, std::ios::binary}; file.empty() || !out)
            error = "Can't write " + anim::markupPathFor(model);
        else
            out.write(text.data(), std::streamsize(text.size()));
        if (error.empty()) {
            animation_->setMarkupText(model, text);
            animation_->applyMarkup(markup, uint32_t(skeleton), true);
            if (preview_->active())
                preview_->setGraph(graphs_->get(graph_));  // (its graph instance relinks the clips)
            created_ = true;  // (a new file: the asset list rescans)
        }
        sendEvents(ui, model, error);
    } else if (type == "editor.anim.close") {
        graph_.clear();
        preview_->close();
    }
    return true;
}

bool AnimEditor::addIk(const std::string& path, const std::string& ik, const std::vector<std::string>& entities, std::string& error, UiLayer& ui)
{
    const std::filesystem::path file = projectFile(projectDir_, path);
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    Json graph;
    try {
        if (file.empty() || !in)
            throw std::runtime_error("can't read " + path);
        graph = Json::parse(text.str());
        if (!graph.isObject())
            throw std::runtime_error(path + " is not a graph");
        if (const Json* has = graph.get("ik"); has && has->isObject() && !has->object.empty())
            throw std::runtime_error(path + " has IK of its own already");
        graph.set("ik", Json::parse(ik));
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    in.close();
    Json& params = graph.child("parameters");
    for (const std::string& name : entities)
        if (!params.get(name.c_str())) {
            Json p = Json::makeObject();
            p.set("type", Json::makeString("entity"));
            params.set(name, std::move(p));
        }
    const auto old = graphs_ ? graphs_->get(path) : nullptr;
    std::ofstream out(file, std::ios::binary);
    const std::string written = graph.dump(2) + "\n";
    if (!out || !out.write(written.data(), std::streamsize(written.size()))) {
        error = "can't write " + path;
        return false;
    }
    out.close();
    if (graphs_) {
        swapTo(path, old);
        if (path == graph_)
            sendGraph(ui, true, true);
    }
    return true;
}

void AnimEditor::swapTo(const std::string& path, const std::shared_ptr<const anim::GraphDefinition>& old)
{
    // The graph as compiled now: the preview's and every character's that played the old one.
    const auto now = graphs_->get(path);
    if (!now || now == old)
        return;
    if (preview_->active() && path == graph_)
        preview_->setGraph(now);
    if (animation_ && old)
        animation_->replaceGraph(old.get(), now);
}

void AnimEditor::sendGraph(UiLayer& ui, bool withText, bool saved)
{
    const auto def = graphs_->get(graph_);
    JsonWriter w;
    w.beginObject().value("type", "editor.anim.graph").value("path", graph_);
    if (saved)
        w.value("saved", true);
    if (withText) {
        const std::filesystem::path file = projectFile(projectDir_, graph_);
        std::ifstream in(file, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        w.value("text", text.str());
    }
    if (def) {
        w.value("name", def->name).value("skeleton", def->skeleton);
        w.beginArray("params");
        for (uint32_t i = 0; i < def->parameters.size(); ++i) {
            const anim::fmt::GraphNode& n = def->nodes[i];
            const char* type = n.type == anim::fmt::NodeType::ParamBool ? "bool" : n.type == anim::fmt::NodeType::ParamId ? "id" : "float";
            w.beginObject().value("name", def->parameters[i]).value("type", type)
                .value("default", n.type == anim::fmt::NodeType::ParamBool ? double(n.flags & 1u) : double(n.f[0])).endObject();
        }
        w.endArray();
    }
    w.beginArray("errors");
    for (const anim::CompileMessage& e : graphs_->errors(graph_))
        w.beginObject().value("node", e.node).value("text", e.text).endObject();
    w.endArray().beginArray("bones");  // the skeleton's: [name, parent] (bone masks)
    if (const anim::AnimData* data = graphs_->data(); data && def && def->skeleton < data->skeletonCount()) {
        const anim::fmt::Skeleton& sk = data->skeleton(def->skeleton);
        const anim::fmt::Bone* b = data->bones(sk);
        for (uint32_t i = 0; i < sk.boneCount; ++i)
            w.beginArray().value(nullptr, data->name(b[i].name)).value(nullptr, int32_t(b[i].parent)).endArray();
    }
    w.endArray().beginArray("clips");  // the skeleton's (by name)
    if (const anim::AnimData* data = graphs_->data(); data && def)
        for (uint32_t c = 0; c < data->clipCount(); ++c)
            if (data->clip(c).skeleton == def->skeleton)
                w.value(nullptr, data->name(data->clip(c).name));
    w.endArray().beginArray("models");
    for (const ScenePak::Library& lib : pak_->libraries())
        if (const int32_t s = modelSkeleton(*pak_, lib); s >= 0 && (!def || uint32_t(s) == def->skeleton))
            w.value(nullptr, lib.source);
    w.endArray().endObject();
    ui.send(w.str());
}

void AnimEditor::sendEvents(UiLayer& ui, const std::string& model, const std::string& error)
{
    JsonWriter w;
    w.beginObject().value("type", "editor.anim.events").value("model", model).value("error", error)
        .value("file", anim::markupPathFor(model));
    const auto& texts = animation_ ? animation_->markupText() : std::map<std::string, std::string>{};
    const auto it = texts.find(model);
    w.value("text", it == texts.end() ? std::string() : it->second);
    // The model's clips: their names and lengths.
    w.beginArray("clips");
    const int32_t skeleton = animation_ ? animation_->modelSkeleton(model) : -1;
    for (const anim::Clip& c : animation_ ? animation_->clips() : std::vector<anim::Clip>{})
        if (skeleton >= 0 && c.skeleton()->index() == uint32_t(skeleton))
            w.beginObject().value("name", c.name()).value("duration", double(c.duration())).value("frames", c.frameCount()).endObject();
    w.endArray().endObject();
    ui.send(w.str());
}

void AnimEditor::sendPreview(UiLayer& ui, const std::string& error)
{
    JsonWriter w;
    const AnimPreview::Options& o = preview_ ? preview_->options : AnimPreview::Options{};
    w.beginObject().value("type", "editor.anim.preview").value("ok", preview_ && preview_->active())
        .value("model", preview_ ? preview_->model() : std::string()).value("error", error);
    w.beginObject("options").value("bones", o.bones).value("grid", o.grid).value("shadows", o.shadows)
        .value("moveRoot", o.moveRoot).value("paused", o.paused).value("speed", double(o.speed)).endObject();
    w.endObject();
    ui.send(w.str());
}

void AnimEditor::update(UiLayer& ui)
{
    if (graph_.empty() || !preview_ || !preview_->active())
        return;
    const uint64_t now = SDL_GetTicksNS();
    if (now - stateNs_ < 100'000'000ull)
        return;
    stateNs_ = now;
    JsonWriter w;
    w.beginObject().value("type", "editor.anim.state").value("time", double(preview_->time()));
    if (anim::GraphInstance* g = preview_->graph()) {
        const anim::GraphDefinition& def = g->definition();
        const int32_t active = g->activeNode(def.root);
        const std::string state = active >= 0 && def.compiled && size_t(active) < def.compiled->nodeIds.size() ? def.compiled->nodeIds[active] : "";
        w.value("state", state);
        w.beginObject("params");
        for (uint32_t i = 0; i < g->parameterCount(); ++i) {
            if (g->parameterType(i) == anim::fmt::NodeType::ParamBool)
                w.value(g->parameterName(i).c_str(), g->getBool(int32_t(i)));
            else if (g->parameterType(i) == anim::fmt::NodeType::ParamFloat)
                w.value(g->parameterName(i).c_str(), double(g->getFloat(int32_t(i))));
        }
        w.endObject();
        // The nodes the last update touched, by their ids (inline constants and paths left out).
        std::vector<anim::GraphInstance::NodeDebug> nodes;
        g->debug(nodes);
        const auto& ids = def.compiled ? def.compiled->nodeIds : std::vector<std::string>{};
        w.beginArray("nodes");
        for (size_t i = 0; i < nodes.size() && i < ids.size(); ++i) {
            const auto& d = nodes[i];
            if (!(d.flags & 2) || ids[i].empty() || ids[i].find('/') != std::string::npos)
                continue;
            w.beginArray().value(nullptr, ids[i]).value(nullptr, uint32_t(d.flags)).value(nullptr, double(d.time))
                .value(nullptr, double(d.progress)).value(nullptr, d.active >= 0 && size_t(d.active) < ids.size() ? ids[d.active] : std::string())
                .endArray();
        }
        w.endArray();
        // Its events since the last of these: [id, weight, percent, kind, flags, active].
        w.beginArray("events");
        for (const anim::SampledEvent& e : preview_->takeEvents())
            w.beginArray().value(nullptr, e.id).value(nullptr, double(e.weight)).value(nullptr, double(e.percent))
                .value(nullptr, uint32_t(e.kind)).value(nullptr, uint32_t(e.flags)).value(nullptr, e.active).endArray();
        w.endArray();
    }
    w.endObject();
    ui.send(w.str());
}

}  // namespace fx
