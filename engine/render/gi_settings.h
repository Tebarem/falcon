#pragma once

// Global illumination's settings (render/probe_gi.h: irradiance probes lit by rays through the
// scene's BVH), the editor's Lighting panel.

#include <cstdint>

namespace fx {

struct GiSettings {
    bool enabled = true;
    float intensity = 1.0f;     // bounced light
    float skyIntensity = 1.0f;  // sky light where the probes' rays get out
    uint32_t sceneRevision = 0; // bump to relight everything (a new scene, a committed edit)
    // Bump to relight only `region` (world min xyz, max xyz) first: what an editor drag moved.
    uint32_t regionRevision = 0;
    float region[6] = {};
    float probeSpacing = 1.0f;    // the finest probe grid's spacing (metres)
    uint32_t probeRays = 262144;  // rays a frame
};

}  // namespace fx
