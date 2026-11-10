#include "ui/ui_layer.h"

#include "core/json.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/profile.h"
#include "gpu/gpu_profiler.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

#if FALCON_UI
#include <cef_lite.h>
#endif

namespace fx {

namespace {

[[maybe_unused]] constexpr float kWheelDelta = 120.0f;  // WHEEL_DELTA: one notch

// SDL keycode -> Windows virtual-key code, which is what Chromium's key events carry (cef-lite
// passes the key through as is). SDL uses lowercase ASCII for letters: 'z' would read as VK_F11.
[[maybe_unused]] uint32_t windowsKeyCode(SDL_Keycode k)
{
    if (k >= SDLK_A && k <= SDLK_Z)
        return uint32_t('A' + (k - SDLK_A));
    if (k >= SDLK_0 && k <= SDLK_9)
        return uint32_t(k);
    if (k >= SDLK_F1 && k <= SDLK_F12)
        return 0x70 + uint32_t(k - SDLK_F1);
    if (k >= SDLK_F13 && k <= SDLK_F24)
        return 0x7C + uint32_t(k - SDLK_F13);
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9)
        return 0x61 + uint32_t(k - SDLK_KP_1);
    switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return 0x0D;
    case SDLK_ESCAPE: return 0x1B;
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_TAB: return 0x09;
    case SDLK_SPACE: return 0x20;
    case SDLK_PAGEUP: return 0x21;
    case SDLK_PAGEDOWN: return 0x22;
    case SDLK_END: return 0x23;
    case SDLK_HOME: return 0x24;
    case SDLK_LEFT: return 0x25;
    case SDLK_UP: return 0x26;
    case SDLK_RIGHT: return 0x27;
    case SDLK_DOWN: return 0x28;
    case SDLK_INSERT: return 0x2D;
    case SDLK_DELETE: return 0x2E;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10;
    case SDLK_LCTRL: case SDLK_RCTRL: return 0x11;
    case SDLK_LALT: case SDLK_RALT: return 0x12;
    case SDLK_LGUI: return 0x5B;
    case SDLK_RGUI: return 0x5C;
    case SDLK_KP_0: return 0x60;
    case SDLK_KP_MULTIPLY: return 0x6A;
    case SDLK_KP_PLUS: return 0x6B;
    case SDLK_KP_MINUS: return 0x6D;
    case SDLK_KP_PERIOD: return 0x6E;
    case SDLK_KP_DIVIDE: return 0x6F;
    case SDLK_SEMICOLON: return 0xBA;
    case SDLK_EQUALS: return 0xBB;
    case SDLK_COMMA: return 0xBC;
    case SDLK_MINUS: return 0xBD;
    case SDLK_PERIOD: return 0xBE;
    case SDLK_SLASH: return 0xBF;
    case SDLK_GRAVE: return 0xC0;
    case SDLK_LEFTBRACKET: return 0xDB;
    case SDLK_BACKSLASH: return 0xDC;
    case SDLK_RIGHTBRACKET: return 0xDD;
    case SDLK_APOSTROPHE: return 0xDE;
    default: return 0;
    }
}

}  // namespace

UiRect UiLayer::viewport(uint32_t width, uint32_t height) const
{
    if (!ui_ || !hasViewport_)
        return {0, 0, width, height};
    // CSS pixels -> framebuffer pixels, clamped to the window; never empty.
    const auto px = [&](float v, uint32_t limit) {
        return uint32_t(std::clamp(std::lround(v * displayScale_), 0l, long(limit)));
    };
    UiRect r;
    r.x = std::min(px(viewport_[0], width), width - 1);
    r.y = std::min(px(viewport_[1], height), height - 1);
    r.w = std::max(1u, std::min(px(viewport_[2], width), width - r.x));
    r.h = std::max(1u, std::min(px(viewport_[3], height), height - r.y));
    return r;
}

bool UiLayer::view(std::string_view name, uint32_t width, uint32_t height, UiRect& out) const
{
    for (const View& v : views_) {
        if (v.name != name || v.rect[2] < 1.0f || v.rect[3] < 1.0f)
            continue;
        const auto px = [&](float x, uint32_t limit) {
            return uint32_t(std::clamp(std::lround(x * displayScale_), 0l, long(limit)));
        };
        out.x = std::min(px(v.rect[0], width), width - 1);
        out.y = std::min(px(v.rect[1], height), height - 1);
        out.w = std::max(1u, std::min(px(v.rect[2], width), width - out.x));
        out.h = std::max(1u, std::min(px(v.rect[3], height), height - out.y));
        return true;
    }
    return false;
}

void UiLayer::handleControl(std::string_view message)
{
    Json m;
    try {
        m = Json::parse(message);
    } catch (const std::exception& e) {
        FX_WARN("ui: bad control message (%s)", e.what());
        return;
    }
    const std::string type = m.getString("type");
    if (type == "ui.layout") {
        const Json* v = m.get("viewport");
        hasViewport_ = v && v->isObject();
        if (hasViewport_) {
            viewport_[0] = float(v->getNumber("x"));
            viewport_[1] = float(v->getNumber("y"));
            viewport_[2] = float(v->getNumber("width"));
            viewport_[3] = float(v->getNumber("height"));
        }
        exclude_.clear();
        if (const Json* list = m.get("exclude"))
            for (const Json& r : list->array)
                exclude_.push_back({float(r.getNumber("x")), float(r.getNumber("y")), float(r.getNumber("width")),
                                    float(r.getNumber("height"))});
        sceneShown_ = m.getBool("scene", true);
        views_.clear();
        if (const Json* list = m.get("views"))
            for (const Json& r : list->array)
                views_.push_back({r.getString("name"), {float(r.getNumber("x")), float(r.getNumber("y")), float(r.getNumber("width")),
                                                        float(r.getNumber("height"))}});
    } else if (type == "ui.chrome") {
        const auto rect = [](const Json* r) {
            return r && r->isObject() ? std::array<float, 4>{float(r->getNumber("x")), float(r->getNumber("y")),
                                                             float(r->getNumber("width")), float(r->getNumber("height"))}
                                      : std::array<float, 4>{};
        };
        titlebar_ = rect(m.get("titlebar"));
        controls_.clear();
        if (const Json* list = m.get("controls"))
            for (const Json& r : list->array)
                controls_.push_back(rect(&r));
    } else if (type == "ui.window") {
        const std::string action = m.getString("action");
        const bool maximized = (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) != 0;
        if (action == "minimize") {
            SDL_MinimizeWindow(window_);
        } else if (action == "maximize") {
            if (maximized)
                SDL_RestoreWindow(window_);
            else
                SDL_MaximizeWindow(window_);
        } else if (action == "close") {
            SDL_Event quit = {};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
        sendWindowState();
    } else if (type == "ui.error") {
        FX_WARN("ui: page error: %s (%s)", m.getString("message").c_str(), m.getString("source").c_str());
    } else if (type == "ui.input") {
        wantsPointer_ = m.getBool("pointer", wantsPointer_);
        wantsKeyboard_ = m.getBool("keyboard", wantsKeyboard_);
        setTextInput(wantsKeyboard_);
    }
}

bool UiLayer::poll(std::string& message)
{
    if (inbox_.empty())
        return false;
    message = std::move(inbox_.front());
    inbox_.pop_front();
    return true;
}

UiStats UiLayer::stats() const
{
    UiStats s;
    s.messagesIn = messagesIn_;
    s.messagesOut = messagesOut_;
#if FALCON_UI
    if (ui_) {
        CefLiteStats c = {};
        c.struct_size = sizeof(c);
        c.version = 1;
        if (cef_lite_get_stats(ui_, &c) == CEF_LITE_STATUS_OK) {
            s.frames = c.frames;
            s.copyMs = c.frames ? double(c.copy_nanoseconds) * 1e-6 / double(c.frames) : 0.0;
            s.copyMaxMs = double(c.copy_max_nanoseconds) * 1e-6;
            s.accelerated = c.accelerated_copies;
        }
    }
#endif
    return s;
}

void UiLayer::setTextInput(bool on)
{
    textInput_ = on;
    applyTextInput();
}

void UiLayer::applyTextInput()
{
    const bool on = textInput_ && !flyDrag();
    if (on == textInputOn_ || !window_)
        return;
    textInputOn_ = on;
    if (on)
        SDL_StartTextInput(window_);
    else
        SDL_StopTextInput(window_);
}

void UiLayer::sendWindowState()
{
    const bool maximized = window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) != 0;
    send(maximized ? R"({"type":"ui.window","maximized":true})" : R"({"type":"ui.window","maximized":false})");
}

void UiLayer::enableWindowChrome()
{
    if (!window_ || chrome_)
        return;
    chrome_ = SDL_SetWindowHitTest(window_, &UiLayer::hitTest, this);
}

// Window coordinates -> CSS pixels; edges resize, the title bar (minus its controls) drags.
SDL_HitTestResult SDLCALL UiLayer::hitTest(SDL_Window* window, const SDL_Point* area, void* data)
{
    auto* self = static_cast<UiLayer*>(data);
    const float x = float(area->x) * self->inputScaleX_ / self->displayScale_;
    const float y = float(area->y) * self->inputScaleY_ / self->displayScale_;
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    const float cw = float(w) * self->inputScaleX_ / self->displayScale_, ch = float(h) * self->inputScaleY_ / self->displayScale_;
    if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED)) {
        constexpr float kEdge = 5.0f;
        const bool l = x < kEdge, r = x >= cw - kEdge, t = y < kEdge, b = y >= ch - kEdge;
        if (t && l) return SDL_HITTEST_RESIZE_TOPLEFT;
        if (t && r) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (b && l) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (b && r) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (t) return SDL_HITTEST_RESIZE_TOP;
        if (b) return SDL_HITTEST_RESIZE_BOTTOM;
        if (l) return SDL_HITTEST_RESIZE_LEFT;
        if (r) return SDL_HITTEST_RESIZE_RIGHT;
    }
    const auto inside = [&](const std::array<float, 4>& r) { return x >= r[0] && y >= r[1] && x < r[0] + r[2] && y < r[1] + r[3]; };
    if (!inside(self->titlebar_))
        return SDL_HITTEST_NORMAL;
    for (const auto& c : self->controls_)
        if (inside(c))
            return SDL_HITTEST_NORMAL;
    return SDL_HITTEST_DRAGGABLE;
}

void UiLayer::pageToWindow(float cssX, float cssY, float& x, float& y) const
{
    x = cssX * displayScale_ / inputScaleX_;
    y = cssY * displayScale_ / inputScaleY_;
}

void UiLayer::updateScale()
{
    int pw = 0, ph = 0, lw = 0, lh = 0;
    SDL_GetWindowSizeInPixels(window_, &pw, &ph);
    SDL_GetWindowSize(window_, &lw, &lh);
    inputScaleX_ = lw > 0 ? float(pw) / float(lw) : 1.0f;
    inputScaleY_ = lh > 0 ? float(ph) / float(lh) : 1.0f;
    const float s = SDL_GetWindowDisplayScale(window_);
    displayScale_ = s > 0.0f ? s : 1.0f;
}

#if FALCON_UI

bool UiLayer::available() { return true; }

bool UiLayer::init(SDL_GPUDevice* device, SDL_Window* window, const char* bundle)
{
    FX_ZONE_CAT("UiLayer::init", UI);
    window_ = window;
    updateScale();
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    width_ = uint32_t(std::max(pw, 1));
    height_ = uint32_t(std::max(ph, 1));

    // Chromium's helper processes are this executable (falcon.exe --type=...: main() hands
    // them to cef_lite_run_subprocess); cef-lite's compositing shaders sit next to it. macOS
    // runs them from their own bundles beside it (FalconUi.cmake: Falcon Helper.app), in
    // Contents/MacOS of Falcon.app, where the base path (Contents/Resources) only links to them.
    const std::string base = SDL_GetBasePath() ? SDL_GetBasePath() : "";
#if defined(__APPLE__)
    const std::string subprocess =
        platform::utf8(platform::executableDir() / "Falcon Helper.app" / "Contents" / "MacOS" / "Falcon Helper");
#else
    const std::string subprocess = platform::utf8(platform::executablePath());
#endif
    const std::string shaders = base + "shaders";
    SDL_PathInfo info = {};
    if (!SDL_GetPathInfo(bundle, &info) || info.type != SDL_PATHTYPE_DIRECTORY) {
        FX_ERROR("ui: bundle directory '%s' not found", bundle);
        return false;
    }

    CefLiteConfig config = {};
    config.struct_size = sizeof(config);
    config.version = CEF_LITE_CONFIG_VERSION;
    config.ui_root = bundle;
    config.subprocess_path = subprocess.c_str();
    config.shader_root = shaders.c_str();
    config.width = width_;
    config.height = height_;
    config.scale = displayScale_;
    config.target_format = device ? SDL_GetGPUSwapchainTextureFormat(device, window) : SDL_GPU_TEXTUREFORMAT_INVALID;
    config.visible = 1;
    const CefLiteStatus status = cef_lite_create(device, &config, &ui_);
    if (status != CEF_LITE_STATUS_OK) {
        FX_ERROR("ui: cef_lite_create failed (%u); is the CEF runtime next to the executable?", status);
        ui_ = nullptr;
        return false;
    }
    FX_INFO("ui: %s (%ux%u, scale %.2f)", bundle, width_, height_, double(displayScale_));
    bundle_ = bundle;
    if (liveReload_)
        bundleWatch_.watch(bundle_);
    return true;
}

void UiLayer::setLiveReload(bool on)
{
    liveReload_ = on;
    bundleWatch_.watch(on && ui_ ? bundle_ : std::string());
}

bool UiLayer::attach(SDL_GPUDevice* device)
{
    if (!ui_)
        return false;
    const CefLiteStatus status = cef_lite_attach_device(ui_, device, SDL_GetGPUSwapchainTextureFormat(device, window_));
    if (status != CEF_LITE_STATUS_OK) {
        FX_ERROR("ui: cef_lite_attach_device failed (%u)", status);
        shutdown();
        return false;
    }
    return true;
}

void UiLayer::shutdown()
{
    bundleWatch_.watch({});
    setTextInput(false);
    if (ui_)
        cef_lite_destroy(ui_);
    ui_ = nullptr;
    inbox_.clear();
    outbox_.clear();
    resizeEdge_ = SDL_HITTEST_NORMAL;
    setEdgeCursor(SDL_HITTEST_NORMAL);
    for (SDL_Cursor*& c : edgeCursors_) {
        if (c)
            SDL_DestroyCursor(c);
        c = nullptr;
    }
}

bool UiLayer::edgeEvent(const SDL_Event& e)
{
#if defined(__APPLE__)
    if (!chrome_ || !window_)
        return false;
    const bool pointer = e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                         e.type == SDL_EVENT_MOUSE_BUTTON_UP;
    if (resizeEdge_ != SDL_HITTEST_NORMAL)
        return pointer;  // (the window follows the mouse in followResize)
    const bool press = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT;
    if ((e.type != SDL_EVENT_MOUSE_MOTION && !press) || pageButtons_ != 0 || gameButtons_ != 0 ||
        (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN))
        return false;
    const SDL_Point at = e.type == SDL_EVENT_MOUSE_MOTION ? SDL_Point{int(e.motion.x), int(e.motion.y)}
                                                          : SDL_Point{int(e.button.x), int(e.button.y)};
    const SDL_HitTestResult edge = hitTest(window_, &at, this);
    const bool onEdge = edge >= SDL_HITTEST_RESIZE_TOPLEFT && edge <= SDL_HITTEST_RESIZE_LEFT;
    setEdgeCursor(onEdge ? edge : SDL_HITTEST_NORMAL);
    if (!onEdge)
        return false;
    if (press) {
        resizeEdge_ = edge;
        SDL_GetGlobalMouseState(&resizeMouse_[0], &resizeMouse_[1]);
        SDL_GetWindowPosition(window_, &resizeRect_[0], &resizeRect_[1]);
        SDL_GetWindowSize(window_, &resizeRect_[2], &resizeRect_[3]);
    }
    return true;
#else
    (void)e;
    return false;
#endif
}

void UiLayer::followResize()
{
    if (resizeEdge_ == SDL_HITTEST_NORMAL || !window_)
        return;
    float mx = 0.0f, my = 0.0f;
    const SDL_MouseButtonFlags buttons = SDL_GetGlobalMouseState(&mx, &my);
    const int dx = int(std::lround(mx - resizeMouse_[0])), dy = int(std::lround(my - resizeMouse_[1]));
    const SDL_HitTestResult e = resizeEdge_;
    const bool left = e == SDL_HITTEST_RESIZE_TOPLEFT || e == SDL_HITTEST_RESIZE_LEFT || e == SDL_HITTEST_RESIZE_BOTTOMLEFT;
    const bool right = e == SDL_HITTEST_RESIZE_TOPRIGHT || e == SDL_HITTEST_RESIZE_RIGHT || e == SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    const bool top = e == SDL_HITTEST_RESIZE_TOPLEFT || e == SDL_HITTEST_RESIZE_TOP || e == SDL_HITTEST_RESIZE_TOPRIGHT;
    const bool bottom = e == SDL_HITTEST_RESIZE_BOTTOMLEFT || e == SDL_HITTEST_RESIZE_BOTTOM || e == SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    int minW = 0, minH = 0;
    SDL_GetWindowMinimumSize(window_, &minW, &minH);
    minW = std::max(minW, 640);  // (the smallest window engine.cpp makes)
    minH = std::max(minH, 400);
    const int* r = resizeRect_;
    int x = r[0], y = r[1], w = r[2], h = r[3];
    if (left) {
        w = std::max(minW, r[2] - dx);
        x = r[0] + r[2] - w;  // (the right edge stays)
    } else if (right) {
        w = std::max(minW, r[2] + dx);
    }
    if (top) {
        h = std::max(minH, r[3] - dy);
        y = r[1] + r[3] - h;  // (the bottom edge stays)
    } else if (bottom) {
        h = std::max(minH, r[3] + dy);
    }
    int cx = 0, cy = 0, cw = 0, ch = 0;
    SDL_GetWindowPosition(window_, &cx, &cy);
    SDL_GetWindowSize(window_, &cw, &ch);
    if (w != cw || h != ch)
        SDL_SetWindowSize(window_, w, h);
    if (x != cx || y != cy)
        SDL_SetWindowPosition(window_, x, y);
    if (!(buttons & SDL_BUTTON_LMASK)) {
        resizeEdge_ = SDL_HITTEST_NORMAL;
        setEdgeCursor(SDL_HITTEST_NORMAL);
    }
}

void UiLayer::setEdgeCursor(SDL_HitTestResult edge)
{
    if (edge == cursorEdge_)
        return;
    cursorEdge_ = edge;
    if (edge < SDL_HITTEST_RESIZE_TOPLEFT || edge > SDL_HITTEST_RESIZE_LEFT) {
        SDL_SetCursor(SDL_GetDefaultCursor());
        return;
    }
    static constexpr SDL_SystemCursor kCursors[8] = {
        SDL_SYSTEM_CURSOR_NW_RESIZE, SDL_SYSTEM_CURSOR_N_RESIZE, SDL_SYSTEM_CURSOR_NE_RESIZE, SDL_SYSTEM_CURSOR_E_RESIZE,
        SDL_SYSTEM_CURSOR_SE_RESIZE, SDL_SYSTEM_CURSOR_S_RESIZE, SDL_SYSTEM_CURSOR_SW_RESIZE, SDL_SYSTEM_CURSOR_W_RESIZE};
    const int i = int(edge) - int(SDL_HITTEST_RESIZE_TOPLEFT);
    if (!edgeCursors_[i])
        edgeCursors_[i] = SDL_CreateSystemCursor(kCursors[i]);
    if (edgeCursors_[i])
        SDL_SetCursor(edgeCursors_[i]);
}

bool UiLayer::handleEvent(const SDL_Event& e)
{
    if (!ui_)
        return true;
    if (edgeEvent(e))
        return false;  // a window edge's (macOS)
    CefLiteInputEvent in = {};
    in.struct_size = sizeof(in);
    in.version = CEF_LITE_INPUT_VERSION;
    in.sequence = ++sequence_;
    in.timestamp_seconds = double(SDL_GetTicksNS()) * 1e-9;
    in.modifiers = uint32_t(SDL_GetModState());
    const auto dispatch = [&] {
        const CefLiteStatus s = cef_lite_dispatch_input(ui_, &in);
        if (s != CEF_LITE_STATUS_OK && s != CEF_LITE_STATUS_NOT_READY && s != CEF_LITE_STATUS_INITIALIZING)
            FX_WARN("ui: input dispatch failed (%u)", s);
    };
    const UiRect vp = viewport(width_, height_);
    // Pointer events inside the game viewport (or during a drag that started there, or while
    // the game holds the mouse in relative mode) belong to the game.
    const auto overPageOverlay = [&](float x, float y) {
        const float cx = x / displayScale_, cy = y / displayScale_;
        for (const auto& r : exclude_)
            if (cx >= r[0] && cy >= r[1] && cx < r[0] + r[2] && cy < r[1] + r[3])
                return true;
        return false;
    };
    // The view under a point ("scene", another view's name, "" none).
    const auto viewAt = [&](float x, float y) -> std::string {
        const float cx = x / displayScale_, cy = y / displayScale_;
        for (const View& v : views_)
            if (cx >= v.rect[0] && cy >= v.rect[1] && cx < v.rect[0] + v.rect[2] && cy < v.rect[1] + v.rect[3])
                return v.name;
        return sceneShown_ && vp.contains(x, y) && !overPageOverlay(x, y) ? "scene" : "";
    };
    const auto gamePointer = [&](float x, float y) {
        if (pageButtons_ != 0)
            return false;
        if (gameButtons_ != 0) {
            pointerView_ = dragView_;
            return true;
        }
        if (SDL_GetWindowRelativeMouseMode(window_)) {
            pointerView_ = "scene";
            return true;
        }
        const std::string at = wantsPointer_ ? std::string() : viewAt(x, y);
        if (!at.empty())
            pointerView_ = at;
        return !at.empty();
    };
    const auto leaveUi = [&] {
        if (!pointerInUi_)
            return;
        pointerInUi_ = false;
        CefLiteInputEvent leave = in;
        leave.kind = CEF_LITE_INPUT_POINTER_MOVE;
        leave.flags = CEF_LITE_INPUT_FLAG_MOUSE_LEAVE;
        cef_lite_dispatch_input(ui_, &leave);
    };

    switch (e.type) {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        updateScale();
        return true;
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
        sendWindowState();
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        in.x = e.motion.x * inputScaleX_;
        in.y = e.motion.y * inputScaleY_;
        if (gamePointer(in.x, in.y)) {
            leaveUi();
            return true;
        }
        pointerView_.clear();  // over the page
        in.kind = CEF_LITE_INPUT_POINTER_MOVE;
        in.delta_x = e.motion.xrel * inputScaleX_;
        in.delta_y = e.motion.yrel * inputScaleY_;
        pointerInUi_ = true;
        dispatch();
        return false;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        in.x = e.button.x * inputScaleX_;
        in.y = e.button.y * inputScaleY_;
        const uint32_t bit = 1u << (e.button.button & 31);
        if (e.button.down ? gamePointer(in.x, in.y) : (gameButtons_ & bit) != 0) {
            if (e.button.down && gameButtons_ == 0)
                dragView_ = pointerView_;  // this view owns the drag
            else if (!e.button.down)
                pointerView_ = dragView_;
            gameButtons_ = e.button.down ? gameButtons_ | bit : gameButtons_ & ~bit;
            applyTextInput();
            return true;
        }
        pageButtons_ = e.button.down ? pageButtons_ | bit : pageButtons_ & ~bit;
        in.kind = CEF_LITE_INPUT_POINTER_BUTTON;
        in.button = e.button.button == SDL_BUTTON_LEFT ? 0u : e.button.button == SDL_BUTTON_MIDDLE ? 1u : 2u;
        in.flags = e.button.down ? 0u : CEF_LITE_INPUT_FLAG_RELEASED;
        in.click_count = e.button.clicks;  // double-click, word / line selection
        dispatch();
        return false;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        in.x = e.wheel.mouse_x * inputScaleX_;
        in.y = e.wheel.mouse_y * inputScaleY_;
        if (gamePointer(in.x, in.y))
            return true;
        in.kind = CEF_LITE_INPUT_POINTER_WHEEL;
        {
            // SDL counts notches (+y away from the user, +x right); Chromium wants a Windows
            // wheel delta, 120 per notch, with +x scrolling left.
            const float sign = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
            in.wheel_x = -e.wheel.x * sign * kWheelDelta;
            in.wheel_y = e.wheel.y * sign * kWheelDelta;
        }
        dispatch();
        return false;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        leaveUi();
        return true;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        // While the game flies the camera (a drag from the viewport), keys are the game's. A right
        // drag's are even with a field focused (the speed box, say): WASD fly, not type into it.
        if (gameButtons_ != 0 && (!wantsKeyboard_ || flyDrag()))
            return true;
        in.kind = CEF_LITE_INPUT_KEY;
        in.key = windowsKeyCode(e.key.key);
        if (in.key == 0)
            return !wantsKeyboard_;  // no Windows equivalent: the page cannot use it
        in.modifiers = uint32_t(e.key.mod);
        in.flags = e.key.down ? 0u : CEF_LITE_INPUT_FLAG_RELEASED;
        dispatch();
        return !wantsKeyboard_;  // page shortcuts and game keys both work unless a field has focus
    case SDL_EVENT_TEXT_INPUT:
        in.kind = CEF_LITE_INPUT_TEXT;
        in.text = reinterpret_cast<const uint8_t*>(e.text.text);
        in.text_length = e.text.text ? uint32_t(SDL_strlen(e.text.text)) : 0u;
        dispatch();
        return false;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        in.kind = CEF_LITE_INPUT_FOCUS;
        in.flags = e.type == SDL_EVENT_WINDOW_FOCUS_GAINED ? 1u : 0u;
        dispatch();
        return true;
    default:
        return true;
    }
}

void UiLayer::pump()
{
    followResize();
    if (!ui_)
        return;
    FX_ZONE_CAT("UiLayer::pump", UI);
    const CefLiteStatus status = cef_lite_pump(ui_);
    if (status == CEF_LITE_STATUS_FAILED || status == CEF_LITE_STATUS_UNSUPPORTED) {
        FX_ERROR("ui: cef-lite stopped (%u); continuing without UI", status);
        shutdown();
        return;
    }
    flush(false);
    // Live reload: the bundle's files changed (and settled). The new page sends its layout and
    // input wishes again; until then nothing is captured.
    if (std::vector<std::string> changed; liveReload_ && bundleWatch_.poll(changed)) {
        FX_INFO("ui: %s changed: reloading the page", changed.front().empty() ? "the bundle" : changed.front().c_str());
        if (cef_lite_reload(ui_) == CEF_LITE_STATUS_OK) {
            ++pageReloads_;
            wantsPointer_ = wantsKeyboard_ = false;
            setTextInput(false);
            outbox_.clear();  // (for the page going away; the new one asks again)
        }
    }
    // Drain the page's messages; layout and input-capture messages are applied right away.
    for (;;) {
        size_t length = 0;
        buffer_.resize(std::max<size_t>(buffer_.size(), 4096));
        CefLiteStatus s = cef_lite_poll_message(ui_, reinterpret_cast<uint8_t*>(buffer_.data()), buffer_.size(), &length);
        if (s == CEF_LITE_STATUS_MESSAGE_TOO_LARGE && length > buffer_.size()) {
            buffer_.resize(length);
            s = cef_lite_poll_message(ui_, reinterpret_cast<uint8_t*>(buffer_.data()), buffer_.size(), &length);
        }
        if (s != CEF_LITE_STATUS_OK || length == 0)
            break;
        ++messagesIn_;
        const std::string_view m(buffer_.data(), length);
        if (m.starts_with("{\"type\":\"ui."))
            handleControl(m);
        else
            inbox_.emplace_back(m);
    }
}

void UiLayer::draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, uint32_t width, uint32_t height)
{
    if (!ui_)
        return;
    FX_ZONE_CAT("UiLayer::draw", UI);
    if (width != width_ || height != height_) {
        width_ = width;
        height_ = height;
        updateScale();
        cef_lite_resize(ui_, width, height, displayScale_);
    }
    SDL_GPUColorTargetInfo color = {};
    color.texture = target;
    color.load_op = SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_STORE;
    FX_GPU_SECTION(cmd, "UI");
    FX_GPU_PASS(cmd, "page composite");
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
    if (!pass)
        return;
    const SDL_FRect rect = {0.0f, 0.0f, float(width), float(height)};
    const CefLiteStatus status = cef_lite_draw(ui_, pass, &rect);
    SDL_EndGPURenderPass(pass);
    if (status != CEF_LITE_STATUS_OK && status != CEF_LITE_STATUS_NOT_READY && status != CEF_LITE_STATUS_INITIALIZING)
        FX_WARN("ui: draw failed (%u)", status);
}

void UiLayer::send(std::string_view json)
{
    if (ui_)
        outbox_.emplace_back(json);
}

void UiLayer::flush(bool force)
{
    if (!ui_ || outbox_.empty())
        return;
    const uint64_t now = SDL_GetTicksNS();
    if (!force && now - lastFlushNs_ < 16'000'000ull)
        return;
    lastFlushNs_ = now;
    const auto deliver = [&](const std::string& payload) {
        const CefLiteStatus s = cef_lite_send_message(ui_, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
        if (s == CEF_LITE_STATUS_MESSAGE_TOO_LARGE)
            FX_WARN("ui: message of %zu bytes is too large for the page", payload.size());
    };
    if (outbox_.size() == 1) {
        deliver(outbox_.front());
    } else {
        // One delivery (one begin frame) for the lot, split to stay well under cef-lite's 1 MB.
        std::string batch;
        for (const std::string& m : outbox_) {
            if (!batch.empty() && batch.size() + m.size() > 512 * 1024) {
                deliver(batch + "]}");
                batch.clear();
            }
            batch += batch.empty() ? R"({"type":"ui.batch","messages":[)" : ",";
            batch += m;
        }
        deliver(batch + "]}");
    }
    messagesOut_ += outbox_.size();
    outbox_.clear();
}

#else  // !FALCON_UI

bool UiLayer::available() { return false; }

bool UiLayer::init(SDL_GPUDevice*, SDL_Window*, const char*)
{
    FX_ERROR("ui: this build has no UI (configure with FALCON_UI=ON and the CEF distribution)");
    return false;
}

bool UiLayer::attach(SDL_GPUDevice*) { return false; }
void UiLayer::setLiveReload(bool) {}
void UiLayer::shutdown() {}
bool UiLayer::handleEvent(const SDL_Event&) { return true; }
void UiLayer::pump() {}
void UiLayer::draw(SDL_GPUCommandBuffer*, SDL_GPUTexture*, uint32_t, uint32_t) {}
void UiLayer::send(std::string_view) {}

#endif

}  // namespace fx
