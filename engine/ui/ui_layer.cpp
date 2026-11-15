#include "ui/ui_layer.h"

#include "core/engine_files.h"
#include "core/json.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_profiler.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>

#if FALCON_UI
#include "ui_composite.shader.h"

#include <ember/ember.h>
#include <ember/ember_resource.h>
#endif

namespace fx {

UiRect UiLayer::viewport(uint32_t width, uint32_t height) const
{
    if (!started_ || !hasViewport_)
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
    s.frames = frames_.load(std::memory_order_acquire);
    s.renderMs = renderMs_.load(std::memory_order_relaxed);
    s.renderMaxMs = renderMaxMs_.load(std::memory_order_relaxed);
    s.messagesIn = messagesIn_.load(std::memory_order_relaxed);
    s.messagesOut = messagesOut_.load(std::memory_order_relaxed);
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

void UiLayer::allowFiles(const std::string& dir)
{
    std::error_code ec;
    const std::string path = platform::utf8(std::filesystem::absolute(std::filesystem::path(std::u8string(dir.begin(), dir.end())), ec));
    if (path.empty())
        return;
    allowed_.push_back(path);
#if FALCON_UI
    if (started_) {  // (the UI thread's runtime takes it; what is sent from now waits for it)
        {
            std::lock_guard lock(queueMutex_);
            allowPending_.push_back(path);
            postPage_ = nullptr;
            work_ = true;
            ++queuedSeq_;
        }
        wake_.notify_all();
    }
#endif
}

#if FALCON_UI

namespace {

// An absolute path as a file: URL (forward slashes; the characters a URL reads otherwise escaped).
std::string fileUrl(const std::string& path)
{
    std::string url = "file:///";
    for (const char c : path) {
        if (c == ' ' || c == '#' || c == '%' || c == '?') {
            url += '%';
            url += "0123456789ABCDEF"[uint8_t(c) >> 4];
            url += "0123456789ABCDEF"[uint8_t(c) & 15];
        } else {
            url += c == '\\' ? '/' : c;
        }
    }
    if (url.starts_with("file:////"))
        url.erase(7, 1);  // (a POSIX path starts with its own slash)
    return url;
}

// falcon://ui/<path>: the page bundles falcon.exe carries (core/engine_files.h), served as the page
// loads them. A folder's URL is its index.html.
bool servePacked(void*, const emb_request_info* req, emb_response* out)
{
    std::string_view url(req->url, req->url_len);
    constexpr std::string_view scheme = "falcon://";
    if (url.size() < scheme.size() || !std::equal(scheme.begin(), scheme.end(), url.begin(), [](char a, char b) {
            return a == char(std::tolower(uint8_t(b)));
        }))
        return false;
    url.remove_prefix(scheme.size());
    url = url.substr(0, url.find_first_of("?#"));
    std::string path;
    for (size_t i = 0; i < url.size(); ++i) {
        if (url[i] == '%' && i + 2 < url.size() && std::isxdigit(uint8_t(url[i + 1])) && std::isxdigit(uint8_t(url[i + 2]))) {
            path += char(std::stoi(std::string(url.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            path += url[i];
        }
    }
    if (path.empty() || path.back() == '/')
        path += "index.html";
    if (!path.starts_with("ui/") || path.find("..") != std::string::npos || path.find('\\') != std::string::npos)
        return false;
    thread_local std::string data;  // (Ember copies it before this returns)
    if (!engine_files::packed().read(path, data)) {
        out->status = 404;
        return true;
    }
    const std::string_view ext = std::string_view(path).substr(std::min(path.rfind('.'), path.size()));
    const char* mime = ext == ".html" ? "text/html"
                       : ext == ".js" ? "text/javascript"
                       : ext == ".css" ? "text/css"
                       : ext == ".json" ? "application/json"
                       : ext == ".svg" ? "image/svg+xml"
                       : ext == ".png" ? "image/png"
                       : ext == ".jpg" || ext == ".jpeg" ? "image/jpeg"
                       : ext == ".woff2" ? "font/woff2"
                       : ext == ".ttf" ? "font/ttf"
                                       : "application/octet-stream";
    out->mime = mime;
    out->mime_len = std::strlen(mime);
    out->data = reinterpret_cast<const uint8_t*>(data.data());
    out->len = data.size();
    return true;
}

void emberLog(void*, emb_log_level level, const char* message)
{
    switch (level) {
    case EMB_LOG_ERROR: FX_ERROR("ui: %s", message); break;
    case EMB_LOG_WARN: FX_WARN("ui: %s", message); break;
    case EMB_LOG_INFO: FX_INFO("ui: %s", message); break;
    default: break;
    }
}

// SDL's modifier state as Ember's EMB_MOD_* bits.
uint32_t modsOf(SDL_Keymod m)
{
    return ((m & SDL_KMOD_SHIFT) ? EMB_MOD_SHIFT : 0u) | ((m & SDL_KMOD_CTRL) ? EMB_MOD_CTRL : 0u) |
           ((m & SDL_KMOD_ALT) ? EMB_MOD_ALT : 0u) | ((m & SDL_KMOD_GUI) ? EMB_MOD_META : 0u);
}

// An SDL mouse button as the DOM numbers it.
emb_button buttonOf(uint8_t b)
{
    switch (b) {
    case SDL_BUTTON_MIDDLE: return EMB_BUTTON_AUXILIARY;
    case SDL_BUTTON_RIGHT: return EMB_BUTTON_SECONDARY;
    case SDL_BUTTON_X1: return EMB_BUTTON_BACK;
    case SDL_BUTTON_X2: return EMB_BUTTON_FORWARD;
    default: return EMB_BUTTON_PRIMARY;
    }
}

// The view's texture size for a size in CSS pixels at a scale (as Ember makes it: rounded up).
uint32_t devicePixels(uint32_t css, float scale)
{
    const float px = std::ceil(float(css) * scale);
    return px >= 1.0f ? uint32_t(px) : 1u;
}

}  // namespace

bool UiLayer::available() { return true; }

bool UiLayer::init(SDL_Window* window, const char* bundle)
{
    FX_ZONE_CAT("UiLayer::init", UI);
    window_ = window;
    updateScale();
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    width_ = uint32_t(std::max(pw, 1));
    height_ = uint32_t(std::max(ph, 1));
    std::vector<std::string> allowed = allowed_;
    packedPages_ = engine_files::isPackedUi(bundle);
    if (packedPages_) {  // falcon://ui/<name>: falcon.exe's own (no files, nothing to watch)
        if (!engine_files::packed().find(std::string(bundle + std::strlen("falcon://")) + "/index.html")) {
            FX_ERROR("ui: %s is not in this executable", bundle);
            return false;
        }
        bundle_ = bundle;
    } else {
        SDL_PathInfo info = {};
        if (!SDL_GetPathInfo(bundle, &info) || info.type != SDL_PATHTYPE_DIRECTORY) {
            FX_ERROR("ui: bundle directory '%s' not found", bundle);
            return false;
        }
        std::error_code ec;
        bundle_ = platform::utf8(std::filesystem::absolute(std::filesystem::path(std::u8string(bundle, bundle + SDL_strlen(bundle))), ec));
        allowed.insert(allowed.begin(), bundle_);
    }
    // The UI thread makes the runtime (Ember is used from one thread); this waits for it.
    {
        std::lock_guard lock(queueMutex_);
        quit_ = false;
        runtimeState_ = 0;
    }
    uiThread_ = std::thread(&UiLayer::uiMain, this, std::move(allowed));
    bool made = false;
    {
        std::unique_lock lock(queueMutex_);
        wake_.wait(lock, [&] { return runtimeState_ != 0; });
        made = runtimeState_ > 0;
    }
    if (!made) {
        uiThread_.join();
        return false;
    }
    started_ = true;
    if (liveReload_ && !packedPages_)
        bundleWatch_.watch(bundle_);
    return true;
}

void UiLayer::setLiveReload(bool on)
{
    liveReload_ = on;
    bundleWatch_.watch(on && started_ && !packedPages_ ? bundle_ : std::string());
}

// The UI thread: what the page says (from its scripts, during an update or an input's dispatch),
// handed to pump() after the update.
void UiLayer::pageMessage(void* user, const char* message, size_t len)
{
    auto* self = static_cast<UiLayer*>(user);
    self->messagesIn_.fetch_add(1, std::memory_order_relaxed);
    self->said_.push_back({std::string(message, len), false});
}

bool UiLayer::attach(const GpuDevice& gpu)
{
    if (!started_)
        return false;
    FX_ZONE_CAT("UiLayer::attach", UI);
    device_ = gpu.get();
    updateScale();
    pageScale_ = displayScale_;
    pageWidth_ = uint32_t(std::max(1l, std::lround(float(width_) / pageScale_)));
    pageHeight_ = uint32_t(std::max(1l, std::lround(float(height_) / pageScale_)));

    // The composite: the page's texture blended (premultiplied) over the frame.
    SDL_GPUShader* vs = gpu.createShader(shaders::ui_composite_vs_main);
    SDL_GPUShader* fs = gpu.createShader(shaders::ui_composite_fs_main);
    SDL_GPUColorTargetDescription color = {};
    color.format = gpu.swapchainFormat();
    color.blend_state.enable_blend = true;
    color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    info.vertex_shader = vs;
    info.fragment_shader = fs;
    composite_ = vs && fs ? SDL_CreateGPUGraphicsPipeline(device_, &info) : nullptr;
    for (SDL_GPUShader* s : {vs, fs})
        if (s)
            SDL_ReleaseGPUShader(device_, s);
    SDL_GPUSamplerCreateInfo si = {};
    si.min_filter = SDL_GPU_FILTER_NEAREST;
    si.mag_filter = SDL_GPU_FILTER_NEAREST;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device_, &si);
    if (!composite_ || !sampler_) {
        FX_ERROR("ui: composite pipeline: %s", SDL_GetError());
        shutdown();
        return false;
    }
    // The page updates at the display's rate (input and messages bring an update sooner).
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
    const float hz = mode && mode->refresh_rate > 0.0f ? std::clamp(mode->refresh_rate, 30.0f, 240.0f) : 60.0f;
    // The UI thread makes the page and loads it now.
    {
        std::lock_guard lock(queueMutex_);
        wantWidth_ = pageWidth_;
        wantHeight_ = pageHeight_;
        wantScale_ = pageScale_;
        periodNs_ = uint64_t(1e9 / double(hz));
        attachRequested_ = true;
        work_ = true;
        ++queuedSeq_;
    }
    wake_.notify_all();
    attached_ = true;
    return true;
}

void UiLayer::shutdown()
{
    bundleWatch_.watch({});
    setTextInput(false);
    // The UI thread ends after the update in progress (it destroys the page and the runtime).
    if (uiThread_.joinable()) {
        {
            std::lock_guard lock(queueMutex_);
            quit_ = true;
        }
        wake_.notify_all();
        uiThread_.join();
    }
    {
        std::lock_guard lock(queueMutex_);
        quit_ = false;
        work_ = false;
        runtimeState_ = 0;
        attachRequested_ = sizeRequested_ = reloadRequested_ = false;
        input_.clear();
        allowPending_.clear();
        outbox_.clear();
        postPage_ = nullptr;
    }
    {
        std::lock_guard lock(outMutex_);
        output_.clear();
    }
    pageFailed_ = false;
    if (device_) {
        if (composite_)
            SDL_ReleaseGPUGraphicsPipeline(device_, composite_);
        if (sampler_)
            SDL_ReleaseGPUSampler(device_, sampler_);
    }
    composite_ = nullptr;
    sampler_ = nullptr;
    device_ = nullptr;
    started_ = false;
    attached_ = false;
    inbox_.clear();
    resizeEdge_ = SDL_HITTEST_NORMAL;
    setEdgeCursor(SDL_HITTEST_NORMAL);
    for (SDL_Cursor*& c : edgeCursors_) {
        if (c)
            SDL_DestroyCursor(c);
        c = nullptr;
    }
}

UiLayer::~UiLayer()
{
    // (a joinable std::thread would terminate the process: engine paths that return early skip shutdown())
    if (uiThread_.joinable()) {
        {
            std::lock_guard lock(queueMutex_);
            quit_ = true;
        }
        wake_.notify_all();
        uiThread_.join();
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

void UiLayer::forwardInput(PageInput input)
{
    {
        std::lock_guard lock(queueMutex_);
        input_.push_back(std::move(input));
        work_ = true;
        ++queuedSeq_;
    }
    wake_.notify_all();
}

bool UiLayer::handleEvent(const SDL_Event& e)
{
    if (!attached_)
        return true;
    if (edgeEvent(e))
        return false;  // a window edge's (macOS)
    // SDL window coordinates -> the page's CSS pixels.
    const float toCss = inputScaleX_ / pageScale_;
    // The page's events go to the UI thread as copies (with the modifiers held now).
    const auto forward = [&] {
        PageInput in;
        in.event = e;
        in.toCss = toCss;
        in.mods = modsOf(SDL_GetModState());
        if (e.type == SDL_EVENT_TEXT_INPUT) {
            in.text = e.text.text ? e.text.text : "";
            in.event.text.text = nullptr;
        } else if (e.type == SDL_EVENT_TEXT_EDITING) {
            in.text = e.edit.text ? e.edit.text : "";
            in.event.edit.text = nullptr;
        }
        forwardInput(std::move(in));
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
        PageInput in;
        in.leave = true;
        forwardInput(std::move(in));
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
        if (gamePointer(e.motion.x * inputScaleX_, e.motion.y * inputScaleY_)) {
            leaveUi();
            return true;
        }
        pointerView_.clear();  // over the page
        pointerInUi_ = true;
        forward();
        return false;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const uint32_t bit = 1u << (e.button.button & 31);
        if (e.button.down ? gamePointer(e.button.x * inputScaleX_, e.button.y * inputScaleY_) : (gameButtons_ & bit) != 0) {
            if (e.button.down && gameButtons_ == 0)
                dragView_ = pointerView_;  // this view owns the drag
            else if (!e.button.down)
                pointerView_ = dragView_;
            gameButtons_ = e.button.down ? gameButtons_ | bit : gameButtons_ & ~bit;
            applyTextInput();
            return true;
        }
        pageButtons_ = e.button.down ? pageButtons_ | bit : pageButtons_ & ~bit;
        forward();
        return false;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (gamePointer(e.wheel.mouse_x * inputScaleX_, e.wheel.mouse_y * inputScaleY_))
            return true;
        forward();
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
        forward();
        return !wantsKeyboard_;  // page shortcuts and game keys both work unless a field has focus
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_TEXT_EDITING:
        forward();
        return false;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        forward();
        return true;
    default:
        return true;
    }
}

// The page follows the window's size and display scale (the UI thread applies it).
void UiLayer::resizePage()
{
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window_, &pw, &ph);
    if (pw <= 0 || ph <= 0)
        return;  // (minimized)
    width_ = uint32_t(pw);
    height_ = uint32_t(ph);
    const bool rescaled = displayScale_ != pageScale_;
    pageScale_ = displayScale_;
    const uint32_t w = uint32_t(std::max(1l, std::lround(float(width_) / pageScale_)));
    const uint32_t h = uint32_t(std::max(1l, std::lround(float(height_) / pageScale_)));
    if (!rescaled && w == pageWidth_ && h == pageHeight_)
        return;
    pageWidth_ = w;
    pageHeight_ = h;
    {
        std::lock_guard lock(queueMutex_);
        wantWidth_ = w;
        wantHeight_ = h;
        wantScale_ = pageScale_;
        sizeRequested_ = true;
        work_ = true;
        ++queuedSeq_;
    }
    wake_.notify_all();
}

void UiLayer::pump()
{
    followResize();
    if (!started_)
        return;
    if (pageFailed_.load(std::memory_order_acquire)) {  // (the UI thread said why)
        FX_WARN("ui: the page could not be made: the UI shuts down");
        shutdown();
        return;
    }
    if (!attached_)
        return;
    FX_ZONE_CAT("UiLayer::pump", UI);
    // Live reload: the bundle's files changed (and settled). The new page sends its layout and
    // input wishes again; until then nothing is captured (see PageOutput::reloaded).
    if (std::vector<std::string> changed; liveReload_ && bundleWatch_.poll(changed)) {
        FX_INFO("ui: %s changed: reloading the page", changed.front().empty() ? "the bundle" : changed.front().c_str());
        {
            std::lock_guard lock(queueMutex_);
            reloadRequested_ = true;
            work_ = true;
            ++queuedSeq_;
        }
        wake_.notify_all();
    }
    resizePage();
    if (lockstepOn_) {
        // (scripted tests) Until the page has taken in everything queued so far, updated and
        // said what it had to: a page that does not get there in 5 s is let be (and said so).
        FX_ZONE_CAT("UiLayer::lockstep wait", UI);
        std::unique_lock lock(queueMutex_);
        const uint64_t queued = queuedSeq_;
        if (!done_.wait_for(lock, std::chrono::seconds(5), [&] {
                return doneSeq_.load(std::memory_order_acquire) >= queued || pageFailed_.load(std::memory_order_acquire);
            }))
            FX_WARN("ui: the page took over 5 s to update (lockstep)");
    }
    // What the page said since the last frame, in order (output_ has an update's output before
    // doneSeq_ moves).
    std::vector<PageOutput> said;
    {
        std::lock_guard lock(outMutex_);
        said.swap(output_);
    }
    for (PageOutput& o : said) {
        if (o.reloaded) {
            ++pageReloads_;
            wantsPointer_ = wantsKeyboard_ = false;
            setTextInput(false);
        } else if (o.message.starts_with("{\"type\":\"ui.")) {
            handleControl(o.message);
        } else {
            inbox_.push_back(std::move(o.message));
        }
    }
}

void UiLayer::draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, uint32_t width, uint32_t height)
{
    if (!attached_ || !composite_)
        return;
    (void)width;
    (void)height;
    // Held from reading the published texture until it is bound: the UI thread replaces (and
    // releases) a published texture only under this lock, and SDL keeps a texture that a command
    // buffer has bound alive until that command buffer is done, submitted or not yet.
    std::lock_guard lock(textureMutex_);
    if (!published_)
        return;
    FX_ZONE_CAT("UiLayer::draw", UI);
    const float tw = float(publishedWidth_), th = float(publishedHeight_);
    const float uniforms[4] = {1.0f / tw, 1.0f / th, tw, th};
    SDL_GPUColorTargetInfo color = {};
    color.texture = target;
    color.load_op = SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_STORE;
    FX_GPU_SECTION(cmd, "UI");
    FX_GPU_PASS(cmd, "page composite");
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
    if (!pass)
        return;
    SDL_BindGPUGraphicsPipeline(pass, composite_);
    const SDL_GPUTextureSamplerBinding binding = {published_, sampler_};
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_PushGPUFragmentUniformData(cmd, 0, uniforms, sizeof(uniforms));
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

void UiLayer::send(std::string_view json)
{
    if (!started_)
        return;
    {
        // Straight to the page when it can take it (Ember allows this from any thread); else in
        // order once it can (it is being made, or a path given to allowFiles is not in yet).
        std::lock_guard lock(queueMutex_);
        if (postPage_) {
            emb_view_post_host_message(postPage_, json.data(), json.size());
            messagesOut_.fetch_add(1, std::memory_order_relaxed);
        } else {
            outbox_.emplace_back(json);
        }
        work_ = true;
        ++queuedSeq_;
    }
    wake_.notify_all();
}

void UiLayer::setLockstep(bool on)
{
    if (on == lockstepOn_)
        return;
    lockstepOn_ = on;
    {
        std::lock_guard lock(queueMutex_);
        lockstep_ = on;
    }
    wake_.notify_all();
}

// ---- The UI thread

void UiLayer::uiMain(std::vector<std::string> allowed)
{
    profiler::nameThread("UI");
    platform::setThreadName("UI");
    emb_runtime_desc rd = {};
    rd.size = sizeof(rd);
    rd.log = emberLog;
    runtime_ = emb_runtime_create(&rd);
    if (runtime_) {
        for (const std::string& dir : allowed)
            emb_runtime_allow_path(runtime_, dir.c_str());
        if (engine_files::packed().has("ui"))
            emb_runtime_set_resource_handler_v2(runtime_, engine_files::kUiScheme, servePacked, nullptr);
    } else
        FX_ERROR("ui: the Ember runtime could not be created");
    {
        std::lock_guard lock(queueMutex_);
        runtimeState_ = runtime_ ? 1 : -1;
    }
    wake_.notify_all();
    if (!runtime_)
        return;

    uint64_t lastUpdate = 0;  // when the last update started
    for (;;) {
        std::vector<PageInput> input;
        std::vector<std::string> allow;
        bool make = false, reload = false, resize = false;
        uint32_t width = 0, height = 0;
        float scale = 1.0f;
        uint64_t taken = 0;  // queuedSeq_ of what this update takes in
        {
            // Paced: an update a display frame after the last, half that when input, messages or
            // requests wait (a page's work then shows sooner); never spinning. In lockstep (a
            // pump is waiting for it) as soon as work waits.
            std::unique_lock lock(queueMutex_);
            for (;;) {
                if (quit_ || !allowPending_.empty() || (!page_ && attachRequested_) || (page_ && work_ && lockstep_))
                    break;
                if (!page_) {
                    wake_.wait(lock);
                    continue;
                }
                const uint64_t due = lastUpdate + (work_ ? periodNs_ / 2 : periodNs_);
                const uint64_t now = SDL_GetTicksNS();
                if (now >= due)
                    break;
                wake_.wait_for(lock, std::chrono::nanoseconds(due - now));
            }
            if (quit_)
                break;
            input.swap(input_);
            allow.swap(allowPending_);
            make = !page_ && attachRequested_;
            reload = std::exchange(reloadRequested_, false);
            resize = std::exchange(sizeRequested_, false) || make;
            width = wantWidth_;
            height = wantHeight_;
            scale = wantScale_;
            taken = queuedSeq_;
            work_ = false;
        }
        for (const std::string& dir : allow)
            emb_runtime_allow_path(runtime_, dir.c_str());
        if (make && !createPage(width, height, scale)) {
            {
                std::lock_guard lock(queueMutex_);
                pageFailed_.store(true, std::memory_order_release);
            }
            done_.notify_all();  // (a lockstep pump stops waiting)
            break;
        }
        if (!page_)
            continue;
        {
            // What was sent while the page could not take it, in order; then send() posts directly.
            std::lock_guard lock(queueMutex_);
            if (!postPage_ && allowPending_.empty()) {
                for (const std::string& m : outbox_)
                    emb_view_post_host_message(page_, m.data(), m.size());
                messagesOut_.fetch_add(outbox_.size(), std::memory_order_relaxed);
                outbox_.clear();
                postPage_ = page_;
            }
        }
        if (reload) {
            // (the mark goes before what the new page says while it loads)
            const size_t at = said_.size();
            if (loadPage())
                said_.insert(said_.begin() + ptrdiff_t(at), PageOutput{{}, true});
        }
        if (resize)
            setPageSize(width, height, scale);
        for (const PageInput& in : input)
            applyInput(in);
        flushOutput();  // (what input made the page say goes now, not after the redraw)
        lastUpdate = SDL_GetTicksNS();
        renderPage();
        flushOutput();
        {
            std::lock_guard lock(queueMutex_);  // (a lockstep pump checks it under the lock)
            doneSeq_.store(taken, std::memory_order_release);  // (after the output: pump takes both together)
        }
        done_.notify_all();
    }
    destroyPage();
    flushOutput();
    emb_runtime_destroy(runtime_);
    runtime_ = nullptr;
}

bool UiLayer::createPage(uint32_t width, uint32_t height, float scale)
{
    FX_ZONE_CAT("UiLayer::createPage", UI);
    emb_view_desc vd = {};
    vd.size = sizeof(vd);
    vd.mode = EMB_VIEW_OFFSCREEN;
    vd.width = width;
    vd.height = height;
    vd.scale = scale;
    vd.device = device_;
    vd.scheme = EMB_SCHEME_DARK;
    vd.transparent = true;  // (the scene shows through where the page paints nothing)
    page_ = emb_view_create(runtime_, &vd);
    if (!page_) {
        FX_ERROR("ui: the page's view could not be created");
        return false;
    }
    viewWidth_ = vd.width;
    viewHeight_ = vd.height;
    viewScale_ = vd.scale;
    emb_view_set_host_channel(page_, "falcon", &UiLayer::pageMessage, this);
    if (!loadPage()) {
        destroyPage();
        return false;
    }
    FX_INFO("ui: %s (%ux%u CSS px, scale %.2f)", bundle_.c_str(), viewWidth_, viewHeight_, double(viewScale_));
    return true;
}

bool UiLayer::loadPage()
{
    const std::string url = packedPages_ ? bundle_ + "/index.html" : fileUrl(bundle_ + "/index.html");
    if (!emb_view_load_url(page_, url.c_str())) {
        FX_ERROR("ui: %s did not load", url.c_str());
        return false;
    }
    return true;
}

void UiLayer::applyInput(const PageInput& in)
{
    if (in.leave) {
        emb_view_mouse_leave(page_);
        return;
    }
    SDL_Event e = in.event;
    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (e.motion.which != SDL_TOUCH_MOUSEID)  // (as Ember's own translation: a touch is its own pointer)
            emb_view_mouse_move(page_, e.motion.x * in.toCss, e.motion.y * in.toCss, in.mods);
        return;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.which != SDL_TOUCH_MOUSEID)
            // (SDL's click count, and the modifiers taken when the event was routed: emb_view_sdl_event
            // would read SDL's state here, on the UI thread, which is already later keys')
            emb_view_mouse_button_clicks(page_, e.button.x * in.toCss, e.button.y * in.toCss, buttonOf(e.button.button),
                                         e.type == SDL_EVENT_MOUSE_BUTTON_DOWN, in.mods, e.button.clicks);
        return;
    case SDL_EVENT_TEXT_INPUT:
        e.text.text = in.text.c_str();
        break;
    case SDL_EVENT_TEXT_EDITING:
        e.edit.text = in.text.c_str();
        break;
    default:
        break;
    }
    emb_view_sdl_event(page_, &e, 0.0f, 0.0f, in.toCss);
}

// A new size or scale replaces the view's texture (Ember releases the old one at once): the last
// picture is copied first and published in its place, so draw() never binds a released texture
// and the window keeps its UI until the page has drawn at the new size.
void UiLayer::setPageSize(uint32_t width, uint32_t height, float scale)
{
    if (width == viewWidth_ && height == viewHeight_ && scale == viewScale_)
        return;
    holdPicture();
    if (scale != viewScale_) {
        viewScale_ = scale;
        emb_view_set_scale(page_, scale);
    }
    if (width != viewWidth_ || height != viewHeight_) {
        viewWidth_ = width;
        viewHeight_ = height;
        emb_view_resize(page_, width, height);
    }
}

void UiLayer::holdPicture()
{
    SDL_GPUTexture* from = nullptr;
    uint32_t w = 0, h = 0;
    {
        std::lock_guard lock(textureMutex_);
        if (!published_ || heldPicture_)
            return;  // (nothing shown yet, or a copy is shown already)
        from = published_;
        w = publishedWidth_;
        h = publishedHeight_;
    }
    SDL_GPUTextureCreateInfo ci = {};
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;  // (an offscreen view's, which a copy needs)
    ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width = w;
    ci.height = h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    SDL_GPUTexture* copy = SDL_CreateGPUTexture(device_, &ci);
    bool copied = false;
    if (SDL_GPUCommandBuffer* cmd = copy ? SDL_AcquireGPUCommandBuffer(device_) : nullptr) {
        SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureLocation src = {};
        src.texture = from;
        SDL_GPUTextureLocation dst = {};
        dst.texture = copy;
        SDL_CopyGPUTextureToTexture(pass, &src, &dst, w, h, 1, false);
        SDL_EndGPUCopyPass(pass);
        copied = SDL_SubmitGPUCommandBuffer(cmd);  // (before it is published: the main thread's use comes after)
    }
    if (!copied && copy) {
        SDL_ReleaseGPUTexture(device_, copy);
        copy = nullptr;
    }
    std::lock_guard lock(textureMutex_);
    published_ = copy;  // (none: the UI is not drawn until the page's next picture)
    heldPicture_ = copy;
}

void UiLayer::renderPage()
{
    // The page's tasks, timers and messages run here, and its picture is drawn into its texture
    // when something changed (a command buffer of the UI thread's own).
    FX_ZONE_CAT("UiLayer::render", UI);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd)
        return;
    const uint64_t t0 = SDL_GetTicksNS();
    const bool changed = emb_view_render(page_, cmd);
    const double ms = double(SDL_GetTicksNS() - t0) * 1e-6;
    renderMs_.store(ms, std::memory_order_relaxed);
    if (ms > renderMaxMs_.load(std::memory_order_relaxed))
        renderMaxMs_.store(ms, std::memory_order_relaxed);
    if (!SDL_SubmitGPUCommandBuffer(cmd)) {
        FX_WARN("ui: the page's frame was not submitted: %s", SDL_GetError());
        return;
    }
    if (!changed)
        return;
    // Published after the submit: a main-thread command buffer that binds it is submitted after
    // it (the lock orders the two), so it samples this picture, complete.
    SDL_GPUTexture* held = nullptr;
    {
        std::lock_guard lock(textureMutex_);
        published_ = emb_view_texture(page_);
        publishedWidth_ = devicePixels(viewWidth_, viewScale_);
        publishedHeight_ = devicePixels(viewHeight_, viewScale_);
        held = std::exchange(heldPicture_, nullptr);
    }
    if (held)
        SDL_ReleaseGPUTexture(device_, held);  // (SDL keeps it while a command buffer still uses it)
    frames_.fetch_add(1, std::memory_order_release);
}

void UiLayer::flushOutput()
{
    if (said_.empty())
        return;
    std::lock_guard lock(outMutex_);
    if (output_.empty()) {
        output_.swap(said_);
    } else {
        for (PageOutput& o : said_)
            output_.push_back(std::move(o));
        said_.clear();
    }
}

void UiLayer::destroyPage()
{
    if (!page_)
        return;
    {
        std::lock_guard lock(queueMutex_);
        postPage_ = nullptr;
    }
    SDL_GPUTexture* held = nullptr;
    {
        std::lock_guard lock(textureMutex_);
        published_ = nullptr;
        held = std::exchange(heldPicture_, nullptr);
    }
    if (held)
        SDL_ReleaseGPUTexture(device_, held);
    emb_view_destroy(page_);
    page_ = nullptr;
}

#else  // !FALCON_UI

bool UiLayer::available() { return false; }

UiLayer::~UiLayer() {}

bool UiLayer::init(SDL_Window*, const char*)
{
    FX_ERROR("ui: this build has no UI (configure with FALCON_UI=ON)");
    return false;
}

bool UiLayer::attach(const GpuDevice&) { return false; }
void UiLayer::setLiveReload(bool) {}
void UiLayer::shutdown() {}
bool UiLayer::edgeEvent(const SDL_Event&) { return false; }
void UiLayer::followResize() {}
void UiLayer::setEdgeCursor(SDL_HitTestResult) {}
bool UiLayer::handleEvent(const SDL_Event&) { return true; }
void UiLayer::pump() {}
void UiLayer::draw(SDL_GPUCommandBuffer*, SDL_GPUTexture*, uint32_t, uint32_t) {}
void UiLayer::send(std::string_view) {}
void UiLayer::setLockstep(bool) {}

#endif

}  // namespace fx
