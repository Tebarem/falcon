#pragma once

// HTML UI through cef-lite (Chromium off-screen rendering, composited with SDL_GPU): the editor
// shell and in-game UI. Built only with FALCON_UI (cef-lite is GPL-3.0); otherwise init() fails
// and the engine runs without UI.
//
// A page talks to the engine with window.falcon.postMessage(json) / window.falcon.onMessage.
// Two message types are consumed here rather than handed to the engine:
//   {"type":"ui.layout","viewport":{"x","y","width","height"},"exclude":[{...}, ...]}  the game's
//       part of the window, in CSS pixels (absent: the whole window). The scene renders and takes
//       pointer input there, except over `exclude` rectangles (page panels floating over it).
//       "scene": false hides it (another view has the space); "views": [{"name", "x", "y",
//       "width", "height"}] are other views the engine draws and gives pointer input (the
//       editor's Animator preview, "anim").
//   {"type":"ui.input","pointer":bool,"keyboard":bool}  the page wants all pointer / keyboard
//       input (an open menu, a focused text field); the game gets none of that kind meanwhile.
//   {"type":"ui.error","message","source"}  a script error on the page (logged as a warning).
// Engine -> page messages are delivered at most every 16 ms: several queued messages arrive as one
// {"type":"ui.batch","messages":[...]} (each element a message object). Every delivery makes
// Chromium begin a frame, and bursts of them crash CEF 151's off-screen renderer.
// With window chrome (a borderless window whose title bar is part of the page):
//   {"type":"ui.chrome","titlebar":{rect},"controls":[{rect}...]}  the draggable title bar, minus
//       its buttons and menus (CSS pixels); the window edges resize.
//   {"type":"ui.window","action":"minimize"|"maximize"|"close"|"query"}  maximize toggles; the
//       layer answers (and reports changes) with {"type":"ui.window","maximized":bool}.
// Live reload: when a file of the bundle changes (saved in an editor, copied over), the page
// reloads (setLiveReload(false): not).

#include "core/dir_watch.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

struct CefLite;

namespace fx {

struct UiRect {
    uint32_t x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const
    {
        return px >= float(x) && py >= float(y) && px < float(x + w) && py < float(y + h);
    }
};

struct UiStats {
    uint64_t frames = 0;           // UI frames received from Chromium
    double copyMs = 0.0;           // average time importing / uploading a frame (main thread)
    double copyMaxMs = 0.0;
    uint64_t accelerated = 0;      // frames copied GPU-side from Chromium's shared texture
    uint64_t messagesIn = 0, messagesOut = 0;
};

class UiLayer {
public:
    static bool available();  // compiled with cef-lite

    // `bundle`: directory with index.html. Non-blocking: Chromium starts in its own processes
    // and the page appears a few frames later; until then draw() leaves the frame untouched.
    // `device` may be null (Chromium starts while the GPU device is still being made): the page
    // loads meanwhile, as long as pump() runs, and attach() gives it the device before draw().
    bool init(SDL_GPUDevice* device, SDL_Window* window, const char* bundle);
    bool attach(SDL_GPUDevice* device);  // false: the UI shut down
    void shutdown();
    bool active() const { return ui_ != nullptr; }

    // Borderless window: the page draws the title bar (see ui.chrome / ui.window above).
    void enableWindowChrome();
    // Reload the page when its bundle's files change (on by default).
    void setLiveReload(bool on);
    uint32_t pageReloads() const { return pageReloads_; }  // live reloads so far
    const std::string& bundle() const { return bundle_; }  // the page's directory

    // Routes one SDL event to the page and returns whether the game should also see it.
    bool handleEvent(const SDL_Event& e);
    // A point in page CSS pixels as SDL window coordinates (tests inject mouse events there).
    void pageToWindow(float cssX, float cssY, float& x, float& y) const;
    SDL_Window* window() const { return window_; }
    // The game may poll the keyboard (camera, scripts) unless a page text field has focus.
    bool gameKeyboard() const { return !ui_ || !wantsKeyboard_; }
    bool wantsPointer() const { return ui_ && wantsPointer_; }  // the pointer is over a control

    // Once per frame after events: runs Chromium's message loop work and drains page messages.
    void pump();
    // Composites the page over `target` (the swapchain, already holding the frame).
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, uint32_t width, uint32_t height);

    void send(std::string_view json);  // queued; see flush()
    // Next page -> engine message (ui.* messages are consumed internally). False when empty.
    bool poll(std::string& message);

    // The game's part of the window in pixels (the whole window without a layout message).
    UiRect viewport(uint32_t width, uint32_t height) const;
    bool sceneShown() const { return sceneShown_; }
    // Another view of the layout, in pixels: false when the page has none of that name.
    bool view(std::string_view name, uint32_t width, uint32_t height, UiRect& out) const;
    // The view the last pointer event that reached the game belongs to ("scene", a view's name):
    // where it is, or the one a drag started in. Keys go to the one under the pointer.
    const std::string& pointerView() const { return pointerView_; }
    UiStats stats() const;

private:
    void handleControl(std::string_view message);
    void updateScale();
    void setTextInput(bool on);
    void applyTextInput();
    // The right button held in a view: the camera flies, and the keys are its (not a focused field's).
    bool flyDrag() const { return (gameButtons_ & (1u << SDL_BUTTON_RIGHT)) != 0; }
    void flush(bool force);
    void sendWindowState();
    static SDL_HitTestResult SDLCALL hitTest(SDL_Window* window, const SDL_Point* area, void* data);
    // macOS: SDL's Cocoa backend acts only on SDL_HITTEST_DRAGGABLE, so the resize edges hitTest
    // reports are handled here: a left press on one starts a resize (edgeEvent, which also
    // shows the resize cursor over them) that followResize, from pump, applies each frame
    // from the global mouse until the button is up. Elsewhere SDL resizes from the hit test.
    bool edgeEvent(const SDL_Event& e);
    void followResize();
    void setEdgeCursor(SDL_HitTestResult edge);
    SDL_HitTestResult resizeEdge_ = SDL_HITTEST_NORMAL;  // the edge being dragged
    SDL_HitTestResult cursorEdge_ = SDL_HITTEST_NORMAL;  // the edge whose cursor is shown
    float resizeMouse_[2] = {};                          // global mouse when the drag started
    int resizeRect_[4] = {};                             // the window then: x, y, w, h
    SDL_Cursor* edgeCursors_[8] = {};                    // per SDL_HITTEST_RESIZE_*

    CefLite* ui_ = nullptr;
    SDL_Window* window_ = nullptr;
    uint32_t width_ = 0, height_ = 0;  // pixels
    float inputScaleX_ = 1.0f, inputScaleY_ = 1.0f, displayScale_ = 1.0f;
    uint64_t sequence_ = 0;
    bool hasViewport_ = false;
    float viewport_[4] = {};           // CSS pixels
    std::vector<std::array<float, 4>> exclude_;  // CSS pixels: page overlays inside the viewport
    bool chrome_ = false;
    std::array<float, 4> titlebar_ = {};          // CSS pixels
    std::vector<std::array<float, 4>> controls_;  // buttons / menus inside the title bar
    bool wantsPointer_ = false, wantsKeyboard_ = false;
    bool textInput_ = false;    // the page has a field focused
    bool textInputOn_ = false;  // SDL's text input (off while flying: an input method may take the keys)
    bool sceneShown_ = true;
    struct View {
        std::string name;
        std::array<float, 4> rect;  // CSS pixels
    };
    std::vector<View> views_;
    std::string pointerView_ = "scene";  // see pointerView()
    std::string dragView_;               // the view a game drag started in
    uint32_t gameButtons_ = 0;         // buttons pressed inside the viewport (game owns the drag)
    uint32_t pageButtons_ = 0;         // buttons pressed on the page (it keeps the drag, e.g. splitters)
    bool pointerInUi_ = false;
    std::deque<std::string> inbox_;
    std::vector<std::string> outbox_;
    uint64_t lastFlushNs_ = 0;
    std::string buffer_;
    uint64_t messagesIn_ = 0, messagesOut_ = 0;
    DirectoryWatch bundleWatch_;  // live reload
    bool liveReload_ = true;
    uint32_t pageReloads_ = 0;
    std::string bundle_;
};

}  // namespace fx
