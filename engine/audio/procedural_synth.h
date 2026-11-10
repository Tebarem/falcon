#pragma once

// Synthesized physics sounds, from a surface's character (assets/surface_sound.h), for surfaces
// without sounds of their own:
//   impact - a few damped modes (inharmonic, ringing longer and brighter on hard surfaces) over a
//            filtered noise transient (most of a hit on gravel or dirt), and a low thud on soft ones;
//   scrape - a loop of band-limited noise with grains (more and coarser on rough surfaces);
//   roll   - a loop of low rumble with bumps.
// And water's (audio/water_audio.h), built from bubbles (each rings at Minnaert's frequency, about
// 3.3 kHz over its radius in mm, and rises in pitch as it nears the surface) and filtered noise:
//   splash     - a slap of noise, a body of churn, a burst of bubbles, a thump and the drops falling
//                back for big ones; `size` (the radius of what fell in, m) sets all of it;
//   slosh      - a loop of water swirling against something, with gurgles;
//   underwater - a loop of low rumble with deep bubbles.
// Mono clips at the mixer's rate. The same seed gives the same samples (tests repeat exactly).

#include "assets/surface_sound.h"
#include "audio/audio_mixer.h"

#include <cstdint>

namespace fx {

AudioClip synthImpact(const SurfaceSound& surface, uint32_t seed);
AudioClip synthScrape(const SurfaceSound& surface, uint32_t seed);
AudioClip synthRoll(const SurfaceSound& surface, uint32_t seed);

AudioClip synthSplash(float size, uint32_t seed);
AudioClip synthSlosh(uint32_t seed);
AudioClip synthUnderwater(uint32_t seed);

}  // namespace fx
