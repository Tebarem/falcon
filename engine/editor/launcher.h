#pragma once

// The launcher: what falcon.exe shows when started with no arguments. Recently opened projects,
// New Project (a name and a folder: an empty project with one empty scene), Open Project (a
// folder picker). Opening one starts the editor on it (falcon.exe --editor --project <dir>,
// another process: one session per process) and closes the launcher once the editor's window
// shows ("Opening…" until then; an editor that stops first is an error). A startup scene whose
// models changed since it was cooked is cooked here first (on a thread, its progress on the
// busy overlay; a failure shown as the error).
//
// Page (ui/launcher) -> engine:
//   launcher.ready                      the state, again
//   launcher.browse {purpose}           a native folder picker; answers launcher.picked {purpose, path}
//   launcher.create {name, location}    a new project in <location>/<name> (cooked on a thread)
//   launcher.open {path}                a project folder or its project.falcon
//   launcher.forget {path}              off the recent list
// Engine -> page: launcher.state {recent [{dir, name, opened, exists}], defaultLocation, busy,
// busyText, error, version}, launcher.picked.
//
// Tests (--launcher-script): the same messages, and test.wait {frames, ms}, test.expect
// {projectExists: dir, sceneExists: path, recentContains: dir, opened: dir, opens: n, error:
// bool, textInput: bool (SDL text input on), value / focused / dialog (of the last test.probe)},
// test.mouse {at: selector, fx, fy, action: click|move} (real SDL mouse events at an element of
// the page), test.probe {at}, test.type {text}, test.key {key} (SDL keyboard events),
// test.capture {path} (the window), test.exit. With --no-relaunch,
// opening only records the project (test.expect opened, opens).

#include "core/json.h"
#include "project/project.h"

#include <SDL3/SDL_video.h>

#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fx {

class UiLayer;

class Launcher {
public:
    struct Init {
        UiLayer* ui = nullptr;
        SDL_Window* window = nullptr;
        bool persistent = true;  // the recent list in the user's app data (false: tests)
        bool relaunch = true;    // open = start the editor (false: tests record it)
    };
    ~Launcher();
    void init(const Init& init);
    bool loadScript(const char* path);
    // Once per frame, after UiLayer::pump().
    void update();
    bool exitRequested() const { return exit_; }
    bool testFailed() const { return testFailed_; }
    std::string takeCaptureRequest() { return std::exchange(capture_, std::string()); }

private:
    void handle(const Json& m);
    void sendState();
    void open(const std::string& path);
    void opened(const Project& p);  // (cooked): the editor started on it
    void fail(const std::string& message);
    bool busy() const { return creating_.valid() || cooking_.valid() || opening_; }
    void click(const Json& step, const Json& probed);  // test.mouse, once the page said where

    UiLayer* ui_ = nullptr;
    SDL_Window* window_ = nullptr;
    bool relaunch_ = true;
    std::unique_ptr<RecentProjects> recent_;
    std::string defaultLocation_, error_;
    bool connected_ = false, exit_ = false, testFailed_ = false;
    uint64_t connectedNs_ = 0;
    std::string opened_;  // --no-relaunch: what would have been opened
    uint32_t opens_ = 0;  // and how many times
    std::string mouse_;   // test.mouse waiting for the page's launcher.probed
    std::string probed_;  // the page's last launcher.probed
    std::string typed_;   // test.type's text (SDL's text event points at it)

    // A project being created (cooking takes about a second): on a thread.
    std::future<std::pair<bool, std::string>> creating_;
    Project created_;
    // A project's startup scene being cooked before the editor opens on it.
    std::future<std::pair<bool, std::string>> cooking_;
    Project cookingProject_;
    std::mutex cookMutex_;
    CookProgress cookProgress_;
    std::string busyText_;
    // The editor started on a project: the launcher stays (busy) until its window shows.
    void* opening_ = nullptr;
    uint64_t openingNs_ = 0;

    // Folder picker answers (on SDL's thread), handed to the main thread.
    std::mutex pickedMutex_;
    std::vector<std::pair<std::string, std::string>> picked_;  // purpose, path

    std::vector<std::string> script_;
    size_t scriptNext_ = 0;
    uint32_t scriptWait_ = 0;
    uint64_t scriptWaitUntilNs_ = 0;
    std::string capture_;
};

}  // namespace fx
