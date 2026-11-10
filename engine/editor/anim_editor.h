#pragma once

// The editor's Animator tab <-> a project's animation graphs and the preview pane.
//   page -> engine  editor.anim.open {path}               (scripts) the page opens a graph in the tab
//                   editor.anim.do {op, ...}              (scripts) an edit of the canvas, by name (animgraph.js run())
//                   editor.anim.graph.open {path}         a .fgraph to edit: compiled, its preview
//                                                         model chosen (the last one, else the
//                                                         first model of its skeleton)
//                   editor.anim.preview.open {model}      the model the preview shows
//                   editor.anim.preview.set {bones, grid, shadows, moveRoot, paused, speed}
//                   editor.anim.preview.restart           the graph from its start
//                   editor.anim.preview.step {seconds}    (while paused)
//                   editor.anim.param {name, value}       a parameter of the preview's graph
//                   editor.anim.graph.compile {path, text} the page's edits: compiled; with no
//                                                         errors the preview and every character
//                                                         playing the graph swap to it (parameter
//                                                         values kept); answered with editor.anim.graph
//                   editor.anim.graph.save {path, text}   written (inside the project only)
//                   editor.anim.graph.create {folder, model} a new graph for a model's skeleton
//                                                         (a state machine playing its first clip)
//                   editor.anim.events.read {model}       a model's clip markup (anim/anim_events.h)
//                   editor.anim.events.save {model, text} written beside the model, onto its clips at once
//                   editor.anim.close                     the tab closed: the preview stops
//   engine -> page  editor.anim.show {path}               open this graph in the tab
//                   editor.anim.graph {path, name, skeleton, errors: [{node, text}],
//                   params: [{name, type, default}], models: [...], clips: [...]} after open, compile
//                   (with "text": the file's, on open), save {saved: true}
//                   editor.anim.preview {ok, model, error, options}
//                   editor.anim.state  ~10 Hz while open: {time, state, params: {name: value},
//                   nodes: [[id, flags (1 initialized, 2 updated), time 0..1, progress (-1: not a
//                   transition), active state's id]]} for the nodes the last update touched,
//                   events: [[id, weight, percent, kind, flags, active]] (every update's since the last)
//                   editor.anim.events {model, file, text, clips: [{name, duration, frames}], error}

#include "anim/anim_graph.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace fx {

class AnimPreview;
class AnimationSystem;
class GraphLibrary;
class ScenePak;
class UiLayer;
struct Json;

class AnimEditor {
public:
    void init(AnimPreview* preview, GraphLibrary* graphs, AnimationSystem* animation, const ScenePak* pak, std::string projectDir);
    // An editor.anim.* message from the page; false if it was not one.
    bool handle(const std::string& type, const Json& m, UiLayer& ui);
    void update(UiLayer& ui);
    bool open() const { return !graph_.empty(); }
    const std::string& graph() const { return graph_; }
    bool takeCreated() { return std::exchange(created_, false); }  // a graph file was written: the asset list rescans
    // An "ik" section (JSON) into the graph file at `path`, with an entity parameter for each of
    // `entities` it lacks; compiled again (the characters playing it, the tab when it shows it).
    // False (`error`) when it can't be read or written or already has IK.
    bool addIk(const std::string& path, const std::string& ik, const std::vector<std::string>& entities, std::string& error, UiLayer& ui);

private:
    void sendGraph(UiLayer& ui, bool withText = false, bool saved = false);
    void swapTo(const std::string& path, const std::shared_ptr<const anim::GraphDefinition>& old);
    void sendPreview(UiLayer& ui, const std::string& error);
    void sendEvents(UiLayer& ui, const std::string& model, const std::string& error);

    AnimPreview* preview_ = nullptr;
    GraphLibrary* graphs_ = nullptr;
    AnimationSystem* animation_ = nullptr;
    const ScenePak* pak_ = nullptr;
    std::string projectDir_;
    std::string graph_;  // the open .fgraph ("" none)
    uint64_t stateNs_ = 0;
    bool created_ = false;
};

}  // namespace fx
