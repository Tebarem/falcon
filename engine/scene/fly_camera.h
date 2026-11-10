#pragma once

#include "core/math.h"

#include <SDL3/SDL_events.h>

#include <algorithm>

namespace fx {

// Everything the renderer needs from a camera for one frame.
struct CameraState {
    float4x4 view, viewProj;
    float3 position, right, up, forward;
    float p00, p11, zNear;
};

// A camera at a world transform (rows of 3x4: it looks along -Z, +Y up) with a vertical field of
// view in degrees: Camera entities.
CameraState cameraStateFromWorld(const float world[3][4], float fovDegrees, float zNear, float aspect);

// Fly camera. Game mode: hold the right mouse button to look, WASD/QE to move, Shift for
// speed, mouse wheel to change base speed.
// Scene-view mode (the editor; Unity's controls): right drag looks (WASD/QE fly and the wheel
// sets the speed only while it is held), middle drag pans, Alt + left drag orbits the pivot,
// Alt + right drag and the wheel zoom toward it. frameOn() glides to a selection.
class FlyCamera {
public:
    // Interior (architecture: stand inside, low, looking down the long axis) or exterior
    // (objects: step back along +Z until the bounds fit, looking at their center).
    void frame(const float boundsMin[3], const float boundsMax[3], bool exterior = false);
    void handleEvent(SDL_Window* window, const SDL_Event& e);
    // Every key and focus event, before the UI layer or the scene view may take it: the fly keys
    // as their events say (update() also reads SDL's keyboard state; either one counts).
    void keyEvent(const SDL_Event& e);
    void update(float dt);
    void dolly(float distance) { position_ += forward() * distance; }

    void setSceneControls(bool on) { sceneControls_ = on; }
    // Glides (~0.25 s) so a sphere fills the view; its center becomes the orbit pivot.
    void frameOn(float3 center, float radius);
    // Glides to look along `direction` at the pivot (orientation gizmo).
    void lookAlong(float3 direction);
    // Orthographic: a 1.5 degree lens from far enough back that what is at the pivot stays the same
    // size (lines stay parallel to within a fraction of a pixel; shadows, GI and picking as ever).
    // Off: the 60 degree lens again, as near.
    void setOrthographic(bool on);
    bool orthographic() const { return fov_ < 10.0f; }
    float fovDegrees() const { return fov_; }
    // A mouse drag is steering the camera (the scene view leaves the mouse alone meanwhile).
    bool busy() const { return looking_ || panning_ || orbiting_ || zooming_; }
    bool flying() const { return looking_; }
    float speed() const { return speed_; }
    // Kept within 0.05 .. 1000 m/s (a saved pose or a field too: below it WASD seems not to move).
    void setSpeed(float s) { speed_ = s >= kMinSpeed ? std::min(s, kMaxSpeed) : kMinSpeed; }
    static constexpr float kMinSpeed = 0.05f, kMaxSpeed = 1000.0f;
    float3 pivot() const { return position_ + forward() * pivotDistance_; }

    // Everything that places the camera (the editor remembers it per scene).
    struct Pose {
        float3 position;
        float yaw, pitch, pivotDistance, speed;
    };
    Pose pose() const { return {position_, yaw_, pitch_, pivotDistance_, speed_}; }
    void setPose(const Pose& p)
    {
        position_ = p.position;
        yaw_ = p.yaw;
        pitch_ = p.pitch;
        pivotDistance_ = p.pivotDistance;
        setSpeed(p.speed);
        glideT_ = 1.0f;
    }

    float3 position() const { return position_; }
    float3 forward() const;
    float4x4 viewProj(float aspect) const;
    CameraState state(float aspect) const;

private:
    void startGlide(float3 position, float yaw, float pitch);
    void changeSpeed(float notches);
    void zoom(float distance);  // toward the pivot (negative: away), within 0.05 m .. 100 km of it
    float3 carry_ = {0, 0, 0};  // what rounding dropped from the last fly step (see update)
    bool held(int key) const;  // a fly key (kFly*), by SDL's state or its events

    enum { kFlyForward, kFlyBack, kFlyRight, kFlyLeft, kFlyUp, kFlyDown, kFlyFast, kFlyKeys };
    bool heldByEvent_[kFlyKeys] = {};
    bool reported_ = false;  // the keyboard's state and events disagreed (said once)

    float3 position_ = {0, 1, 5};
    float yaw_ = 0.0f, pitch_ = 0.0f;
    float speed_ = 5.0f;
    float nearPlane_ = 0.05f;
    float fov_ = 60.0f;  // vertical, degrees (orthographic: 1.5)
    float pivotDistance_ = 10.0f;
    bool looking_ = false, panning_ = false, orbiting_ = false, zooming_ = false;
    bool sceneControls_ = false;
    // Glide: from -> to over kGlideSeconds.
    float glideT_ = 1.0f;
    float3 glideFrom_ = {}, glideTo_ = {};
    float glideYaw_[2] = {}, glidePitch_[2] = {};
};

}  // namespace fx
