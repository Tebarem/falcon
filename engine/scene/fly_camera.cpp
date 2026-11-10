#include "scene/fly_camera.h"

#include "core/log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace fx {

namespace {
constexpr float kGlideSeconds = 0.25f;
}  // namespace

void FlyCamera::frame(const float lo[3], const float hi[3], bool exterior)
{
    const float3 extent = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
    const float size = std::max({extent.x, extent.y, extent.z});
    if (exterior) {
        const float3 center = {lo[0] + extent.x * 0.5f, lo[1] + extent.y * 0.5f, lo[2] + extent.z * 0.5f};
        const float radius = length(extent) * 0.5f;
        pitch_ = -0.25f;  // slightly from above
        yaw_ = 0.0f;      // look toward -Z
        position_ = center - forward() * (radius * 1.8f);
        pivotDistance_ = radius * 1.8f;
        speed_ = std::max(size * 0.25f, 0.5f);
        nearPlane_ = std::clamp(size * 1e-3f, 0.005f, 0.1f);
        return;
    }
    // Stand near the middle, low, looking down the longest horizontal axis.
    position_ = {lo[0] + extent.x * 0.5f, lo[1] + extent.y * 0.2f, lo[2] + extent.z * 0.5f};
    if (extent.x >= extent.z) {
        position_.x = lo[0] + extent.x * 0.15f;
        yaw_ = -kPi * 0.5f;  // look toward +X
    } else {
        position_.z = hi[2] - extent.z * 0.15f;
        yaw_ = 0.0f;  // look toward -Z
    }
    pitch_ = 0.0f;
    pivotDistance_ = std::max(size * 0.1f, 1.0f);
    speed_ = std::max(size * 0.1f, 0.5f);
    nearPlane_ = std::clamp(size * 1e-4f, 0.005f, 0.1f);
}

float3 FlyCamera::forward() const
{
    return {-std::sin(yaw_) * std::cos(pitch_), std::sin(pitch_), -std::cos(yaw_) * std::cos(pitch_)};
}

void FlyCamera::startGlide(float3 position, float yaw, float pitch)
{
    glideFrom_ = position_;
    glideTo_ = position;
    // Turn the short way round.
    while (yaw - yaw_ > kPi)
        yaw -= 2.0f * kPi;
    while (yaw - yaw_ < -kPi)
        yaw += 2.0f * kPi;
    glideYaw_[0] = yaw_;
    glideYaw_[1] = yaw;
    glidePitch_[0] = pitch_;
    glidePitch_[1] = pitch;
    glideT_ = 0.0f;
}

void FlyCamera::frameOn(float3 center, float radius)
{
    // Fit the sphere in the vertical field of view, with a margin.
    const float distance = std::max(radius, 0.05f) / std::sin(fov_ * 0.5f * kPi / 180.0f) * 1.15f;
    pivotDistance_ = distance;
    startGlide(center - forward() * distance, yaw_, pitch_);
}

void FlyCamera::setOrthographic(bool on)
{
    const float to = on ? 1.5f : 60.0f;
    if (to == fov_)
        return;
    // The pivot kept where it is on screen and the same size: further back by the lenses' ratio.
    const float3 p = pivot();
    pivotDistance_ *= std::tan(fov_ * 0.5f * kPi / 180.0f) / std::tan(to * 0.5f * kPi / 180.0f);
    fov_ = to;
    position_ = p - forward() * pivotDistance_;
    glideT_ = 1.0f;
}

void FlyCamera::lookAlong(float3 direction)
{
    const float3 d = normalize(direction);
    const float3 p = pivot();
    const float pitch = std::asin(std::clamp(d.y, -0.999f, 0.999f));
    const float yaw = std::fabs(d.x) + std::fabs(d.z) > 1e-4f ? std::atan2(-d.x, -d.z) : yaw_;
    const float3 f = {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
    startGlide(p - f * pivotDistance_, yaw, pitch);
}

// The wheel's notches (fractions of one from high-resolution and free-spinning wheels, which send
// many small events; 0 from a sideways scroll) change the fly speed by 1.25x a notch. Counting
// each event as a whole notch, and a sideways one as a notch down, let a touch of such a wheel
// take the speed to nothing: the view still turned, but WASD no longer moved it.
void FlyCamera::changeSpeed(float notches)
{
    setSpeed(speed_ * std::pow(1.25f, std::clamp(notches, -4.0f, 4.0f)));
}

void FlyCamera::handleEvent(SDL_Window* window, const SDL_Event& e)
{
    if (sceneControls_) {
        const bool alt = (SDL_GetModState() & SDL_KMOD_ALT) != 0;
        switch (e.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            if (e.button.button == SDL_BUTTON_RIGHT) {
                if (down)
                    (alt ? zooming_ : looking_) = true;
                else
                    zooming_ = looking_ = false;
            } else if (e.button.button == SDL_BUTTON_MIDDLE) {
                panning_ = down;
            } else if (e.button.button == SDL_BUTTON_LEFT) {
                if (down && alt)
                    orbiting_ = true;
                else if (!down)
                    orbiting_ = false;
            } else {
                break;
            }
            SDL_SetWindowRelativeMouseMode(window, busy());
            if (busy())
                glideT_ = 1.0f;  // the user takes over
            break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
            const float dx = e.motion.xrel, dy = e.motion.yrel;
            if (looking_ || orbiting_) {
                const float3 p = pivot();
                yaw_ -= dx * 0.0035f;
                pitch_ = std::clamp(pitch_ - dy * 0.0035f, -1.55f, 1.55f);
                if (orbiting_)
                    position_ = p - forward() * pivotDistance_;
            } else if (panning_) {
                const float3 f = forward();
                const float3 right = normalize(cross(f, {0, 1, 0}));
                const float3 up = cross(right, f);
                const float k = pivotDistance_ * 0.0016f * std::tan(fov_ * 0.5f * kPi / 180.0f) / std::tan(30.0f * kPi / 180.0f);
                position_ += right * (-dx * k) + up * (dy * k);
            } else if (zooming_) {
                zoom((dx - dy) * pivotDistance_ * 0.004f);
            }
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            if (looking_) {
                changeSpeed(e.wheel.y);
            } else {
                // Zoom toward the pivot, a fraction of the distance per notch.
                zoom(pivotDistance_ * 0.15f * e.wheel.y);
                glideT_ = 1.0f;
            }
            break;
        default: break;
        }
        return;
    }
    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e.button.button == SDL_BUTTON_RIGHT) {
            looking_ = true;
            SDL_SetWindowRelativeMouseMode(window, true);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button == SDL_BUTTON_RIGHT) {
            looking_ = false;
            SDL_SetWindowRelativeMouseMode(window, false);
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (looking_) {
            yaw_ -= e.motion.xrel * 0.0025f;
            pitch_ = std::clamp(pitch_ - e.motion.yrel * 0.0025f, -1.55f, 1.55f);
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        changeSpeed(e.wheel.y);
        break;
    default: break;
    }
}

void FlyCamera::update(float dt)
{
    if (glideT_ < 1.0f) {
        glideT_ = std::min(glideT_ + dt / kGlideSeconds, 1.0f);
        const float t = glideT_ * glideT_ * (3.0f - 2.0f * glideT_);
        position_ = glideFrom_ + (glideTo_ - glideFrom_) * t;
        yaw_ = glideYaw_[0] + (glideYaw_[1] - glideYaw_[0]) * t;
        pitch_ = glidePitch_[0] + (glidePitch_[1] - glidePitch_[0]) * t;
    }
    // Scene view: keys fly only while the right button is held (they are tool keys otherwise).
    if (sceneControls_ && !looking_)
        return;
    const float3 f = forward();
    const float3 right = normalize(cross(f, {0, 1, 0}));
    float3 move = {0, 0, 0};
    if (held(kFlyForward)) move += f;
    if (held(kFlyBack)) move += f * -1.0f;
    if (held(kFlyRight)) move += right;
    if (held(kFlyLeft)) move += right * -1.0f;
    if (held(kFlyUp)) move += float3{0, 1, 0};
    if (held(kFlyDown)) move += float3{0, -1, 0};
    const float boost = held(kFlyFast) ? 4.0f : 1.0f;
    if (move.x == 0.0f && move.y == 0.0f && move.z == 0.0f) {
        carry_ = {0, 0, 0};
        return;
    }
    // Far from the origin a frame's step can be under half a float's spacing there (5 m/s at
    // 1,000 fps is 5 mm; at a million metres the spacing is 6 cm) and round away to nothing: the
    // view turned but WASD never moved it (until a speed of 100,000). What the rounding dropped is
    // carried into the next frame's step, so the camera moves at its speed anywhere.
    const float3 step = normalize(move) * (speed_ * boost * dt) + carry_;
    const float3 before = position_;
    position_ += step;
    carry_ = step - (position_ - before);
}

// The farthest the wheel or an Alt + right drag zooms out from the pivot (100 km), so neither can
// take the camera to where a step rounds away (above) and nothing of the scene is in sight.
static constexpr float kMaxPivotDistance = 100000.0f;

void FlyCamera::zoom(float d)
{
    d = std::max(d, pivotDistance_ - kMaxPivotDistance);
    position_ += forward() * std::min(d, pivotDistance_ * 0.9f);
    pivotDistance_ = std::clamp(pivotDistance_ - d, 0.05f, kMaxPivotDistance);
}

namespace {
// The fly keys by where they are (W A S D Q E, left Shift), whatever the layout.
constexpr SDL_Scancode kFlyScancodes[] = {SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_A,
                                          SDL_SCANCODE_E, SDL_SCANCODE_Q, SDL_SCANCODE_LSHIFT};
}  // namespace

bool FlyCamera::held(int key) const
{
    return SDL_GetKeyboardState(nullptr)[kFlyScancodes[key]] || heldByEvent_[key];
}

// A key's events and SDL's keyboard state normally agree. On a friend's PC, right-drag turned the
// view but WASD never moved it, and every other cause was ruled out, so flying takes either one,
// and the editor's console says once when they disagree (or the window lacks keyboard focus).
void FlyCamera::keyEvent(const SDL_Event& e)
{
    if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        std::fill(std::begin(heldByEvent_), std::end(heldByEvent_), false);
        return;
    }
    if (e.type != SDL_EVENT_KEY_DOWN && e.type != SDL_EVENT_KEY_UP)
        return;
    for (int k = 0; k < kFlyKeys; ++k) {
        if (e.key.scancode != kFlyScancodes[k])
            continue;
        heldByEvent_[k] = e.key.down;
        if (e.key.down && looking_ && !reported_ && !SDL_GetKeyboardState(nullptr)[kFlyScancodes[k]]) {
            reported_ = true;
            SDL_Window* focus = SDL_GetKeyboardFocus();
            FX_WARN("fly: %s is down by its event but up in SDL's keyboard state (keyboard focus: %s); flying uses the event",
                    SDL_GetScancodeName(kFlyScancodes[k]), focus ? "this window" : "none");
        }
    }
}

float4x4 FlyCamera::viewProj(float aspect) const
{
    return state(aspect).viewProj;
}

CameraState cameraStateFromWorld(const float world[3][4], float fovDegrees, float zNear, float aspect)
{
    CameraState s;
    const float3 back = normalize(float3{world[0][2], world[1][2], world[2][2]});
    const float3 up = normalize(float3{world[0][1], world[1][1], world[2][1]});
    s.forward = back * -1.0f;
    s.right = normalize(cross(s.forward, up));
    s.up = cross(s.right, s.forward);
    s.position = {world[0][3], world[1][3], world[2][3]};
    s.view = viewFromBasis(s.right, s.up, s.forward, s.position);
    const float4x4 proj = perspectiveReverseZ(std::clamp(fovDegrees, 1.0f, 179.0f) * kPi / 180.0f, aspect, std::max(zNear, 1e-4f));
    s.viewProj = proj * s.view;
    s.p00 = proj.m[0][0];
    s.p11 = proj.m[1][1];
    s.zNear = std::max(zNear, 1e-4f);
    return s;
}

CameraState FlyCamera::state(float aspect) const
{
    CameraState s;
    s.forward = forward();
    s.right = normalize(cross(s.forward, {0, 1, 0}));
    s.up = cross(s.right, s.forward);
    s.position = position_;
    s.view = viewFromBasis(s.right, s.up, s.forward, position_);
    const float4x4 proj = perspectiveReverseZ(fov_ * kPi / 180.0f, aspect, orthographic() ? std::max(nearPlane_, pivotDistance_ * 0.05f) : nearPlane_);
    s.viewProj = proj * s.view;
    s.p00 = proj.m[0][0];
    s.p11 = proj.m[1][1];
    s.zNear = orthographic() ? std::max(nearPlane_, pivotDistance_ * 0.05f) : nearPlane_;
    return s;
}

}  // namespace fx
