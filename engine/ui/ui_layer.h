#pragma once

// HTML UI through Ember (external/ember: HTML, CSS and JavaScript drawn with SDL_GPU on the engine's
// own device), composited over the frame: the editor shell, the launcher and in-game UI. Built with
// FALCON_UI (on by default); without it init() fails and the engine runs without UI.
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
// Engine -> page messages are delivered in order, each as one onMessage call, during the page's
// next update.
//
// Threads. The page (Ember's runtime and view) lives on a thread of its own, "UI": it updates
// the page at the display's rate (sooner when input or messages arrive), records its picture
// into the view's texture and submits it, then publishes that texture. The main thread never
// waits on page work: handleEvent() routes events and queues the page's for the UI thread,
// pump() applies what the page said since the last frame (in order), and draw() blends the
// last published picture. Both submit to the same SDL_GPUDevice queue; a texture is published
// only after the command buffer that drew it was submitted.
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
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

struct emb_runtime;
struct emb_view;

namespace fx {

class GpuDevice;

struct UiRect {
    uint32_t x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const
    {
        return px >= float(x) && py >= float(y) && px < float(x + w) && py < float(y + h);
    }
};

struct UiStats {
    uint64_t frames = 0;        // page frames that changed the picture (submitted and published)
    double renderMs = 0.0;      // the last page update (UI thread): style, layout, display list and its GPU recording
    double renderMaxMs = 0.0;   // the slowest since the start
    uint64_t messagesIn = 0, messagesOut = 0;
};

class UiLayer {
public:
    UiLayer() = default;
    ~UiLayer();  // stops the UI thread if shutdown() did not (an early exit)
    UiLayer(const UiLayer&) = delete;
    UiLayer& operator=(const UiLayer&) = delete;
    static bool available();  // compiled with Ember

    // `bundle`: directory with index.html. The page is made in attach(), once the GPU device exists
    // (Ember draws with it); init() starts the UI thread and what does not need the device there
    // (the runtime, the system font index).
    bool init(SDL_Window* window, const char* bundle);
    // The UI thread makes the page and loads it (attach does not wait). False: the UI shut down;
    // a page that fails later shuts the UI down from pump() (active() turns false).
    bool attach(const GpuDevice& gpu);
    void shutdown();  // waits for the page's update in progress, if any
    bool active() const { return started_; }

    // Lets the page (and its iframes) read the files under `dir` through file: URLs, as it can its
    // own bundle (the project's pages).
    void allowFiles(const std::string& dir);
    // Borderless window: the page draws the title bar (see ui.chrome / ui.window above).
    void enableWindowChrome();
    // Reload the page when its bundle's files change (on by default).
    void setLiveReload(bool on);
    uint32_t pageReloads() const { return pageReloads_; }  // live reloads so far
    const std::string& bundle() const { return bundle_; }  // the page's directory (or falcon://ui/<name>)

    // Routes one SDL event to the page and returns whether the game should also see it.
    bool handleEvent(const SDL_Event& e);
    // A point in page CSS pixels as SDL window coordinates (tests inject mouse events there).
    void pageToWindow(float cssX, float cssY, float& x, float& y) const;
    SDL_Window* window() const { return window_; }
    // The game may poll the keyboard (camera, scripts) unless a page text field has focus.
    bool gameKeyboard() const { return !started_ || !wantsKeyboard_; }
    bool wantsPointer() const { return started_ && wantsPointer_; }  // the pointer is over a control

    // Once per frame after events: takes what the page said since the last call (in order; ui.*
    // messages are applied here, the rest wait for poll), and asks the UI thread for a reload or a
    // new size. Never waits on the page.
    void pump();
    // Composites the page's last published picture over `target` (the swapchain, already holding
    // the frame).
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, uint32_t width, uint32_t height);

    void send(std::string_view json);  // delivered on the page's next update (main thread)
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
    // For scripted tests, which count frames: on, each pump() first waits until the page has taken
    // in what was queued for it (input, messages, requests), updated and said what it had to say,
    // as when the page updated on the main thread (a frame then sees the page's answer to the
    // last). Off (the default) the main thread never waits for the page.
    void setLockstep(bool on);

private:
    // An SDL event for the page (or the pointer leaving it), copied when it was routed: text
    // events' strings are SDL's only until the next poll, and the keyboard modifiers are taken
    // then (Ember would read SDL's state on the UI thread, which by then may be later keys').
    struct PageInput {
        SDL_Event event = {};
        std::string text;   // SDL_EVENT_TEXT_INPUT / TEXT_EDITING
        uint32_t mods = 0;  // EMB_MOD_* at routing time (mouse events)
        float toCss = 1.0f; // SDL window coordinates -> CSS pixels
        bool leave = false; // emb_view_mouse_leave rather than the event
    };
    // A page -> engine message, or the mark that a reload happened there (what the page wished
    // before it no longer holds).
    struct PageOutput {
        std::string message;
        bool reloaded = false;
    };

    static void pageMessage(void* user, const char* message, size_t len);
    void handleControl(std::string_view message);
    void updateScale();
    void resizePage();
    void forwardInput(PageInput input);  // main thread: queued for the UI thread
    void setTextInput(bool on);
    void applyTextInput();
    // The right button held in a view: the camera flies, and the keys are its (not a focused field's).
    bool flyDrag() const { return (gameButtons_ & (1u << SDL_BUTTON_RIGHT)) != 0; }
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

    // ---- UI thread (uiThread_): Ember is used from it only.
    void uiMain(std::vector<std::string> allowed);
    bool createPage(uint32_t width, uint32_t height, float scale);  // the view, its host channel, the first load
    bool loadPage();
    void applyInput(const PageInput& input);
    void setPageSize(uint32_t width, uint32_t height, float scale);
    void holdPicture();                 // a copy published while the view's texture is replaced
    void renderPage();                  // one update: render, submit, publish
    void flushOutput();                 // what the page said -> pump
    void destroyPage();
    emb_runtime* runtime_ = nullptr;
    emb_view* page_ = nullptr;          // the page, once the UI thread made it
    uint32_t viewWidth_ = 0, viewHeight_ = 0;  // the view's size (CSS pixels) and scale as set
    float viewScale_ = 0.0f;
    std::vector<PageOutput> said_;      // since the last flushOutput

    // ---- main thread -> UI thread (queueMutex_)
    std::thread uiThread_;
    std::mutex queueMutex_;
    std::condition_variable wake_;
    bool quit_ = false;
    bool work_ = false;                 // input, messages or a request since the UI thread's last update
    uint64_t queuedSeq_ = 0;            // counts what was queued
    bool lockstep_ = false;             // setLockstep: update as soon as work waits (no pacing)
    int runtimeState_ = 0;              // 1 the runtime is made, -1 it failed (init waits for it)
    bool attachRequested_ = false;      // the device is there: make the page
    std::vector<PageInput> input_;
    std::vector<std::string> allowPending_;  // allowFiles after init
    std::vector<std::string> outbox_;   // send() while the page cannot take them (none yet, a path pending)
    emb_view* postPage_ = nullptr;      // send() posts to it directly (Ember allows that from any thread)
    bool sizeRequested_ = false;
    uint32_t wantWidth_ = 0, wantHeight_ = 0;  // CSS pixels
    float wantScale_ = 1.0f;
    bool reloadRequested_ = false;
    uint64_t periodNs_ = 16'666'667;    // the page's update rate: the display's

    // ---- UI thread -> main thread
    std::mutex outMutex_;
    std::vector<PageOutput> output_;    // taken by pump, in order
    std::mutex textureMutex_;           // draw() holds it from reading the texture until it is bound
    SDL_GPUTexture* published_ = nullptr;  // the picture draw() blends: the view's texture or holdPicture's copy
    uint32_t publishedWidth_ = 0, publishedHeight_ = 0;  // its pixels
    SDL_GPUTexture* heldPicture_ = nullptr;  // holdPicture's copy (released once the view's is published)
    std::atomic<bool> pageFailed_{false};
    std::atomic<uint64_t> doneSeq_{0};  // queuedSeq_ as of the UI thread's last finished update
    std::condition_variable done_;      // doneSeq_ moved (with queueMutex_: a lockstep pump waits on it)
    std::atomic<uint64_t> frames_{0};
    std::atomic<double> renderMs_{0.0}, renderMaxMs_{0.0};
    std::atomic<uint64_t> messagesIn_{0}, messagesOut_{0};

    // ---- main thread
    bool started_ = false;            // init() succeeded (the page comes with attach)
    bool attached_ = false;           // attach() handed the device over: events go to the page
    bool lockstepOn_ = false;         // setLockstep's (lockstep_ as the main thread set it)
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUGraphicsPipeline* composite_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_Window* window_ = nullptr;
    uint32_t width_ = 0, height_ = 0;  // pixels
    uint32_t pageWidth_ = 0, pageHeight_ = 0;  // the page's size in CSS pixels
    float pageScale_ = 0.0f;
    float inputScaleX_ = 1.0f, inputScaleY_ = 1.0f, displayScale_ = 1.0f;
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
    std::vector<std::string> allowed_; // directories the page may read (allowFiles before init)
    DirectoryWatch bundleWatch_;  // live reload
    bool liveReload_ = true;
    uint32_t pageReloads_ = 0;
    std::string bundle_;
    bool packedPages_ = false;  // bundle_ is falcon://ui/<name>: falcon.exe's own (engine_files.h)
};

}  // namespace fx
