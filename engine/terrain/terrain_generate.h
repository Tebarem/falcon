#pragma once

// Terrain generation: a stack of steps that shape heights and paint materials, run on the GPU
// (render/terrain_generator.h) over the whole terrain or a region of it, then written into the
// terrain like a brush stroke (one undo step). The stack is kept in terrain.json and can be run
// again (the same seed gives the same ground).
//
// Steps, in order, each over what the steps before it made:
//   noise    fractal noise (fbm, ridged, billow, eroded: fbm whose detail fades on slopes), or
//            Voronoi (cells: rounded hills, crags: sharp ridges between cells, mesas: flat-topped
//            cells at random heights); feature size, height, octaves, domain warp
//   terrace  steps of a height (with a noise-broken strata pattern)
//   plateau  heights above a level pressed down toward it (soft knee)
//   smooth   a blur of a radius
//   erode    hydraulic erosion (rain, water flowing over the ground carrying sediment, a pipe
//            model on a grid of `cellSize`) and thermal erosion (slopes steeper than the talus
//            angle slump); it leaves flow and deposit masks for the paint steps after it
//   paint    a material layer where the step's masks allow (auto-painting by rules)
// Every step has a blend (add, max, min, replace, multiply) with an amount, and masks: height,
// slope and curvature ranges with soft edges, a noise mask, and for paint the flow (rivers,
// gullies) and deposit (fans, valley floors) of the last erosion.

#include <cstdint>
#include <string>
#include <vector>

namespace fx {

struct Json;
class JsonWriter;

enum class GenOp : uint32_t { Noise, Terrace, Plateau, Smooth, Erode, Paint, Count };
enum class GenNoise : uint32_t { Fbm, Ridged, Billow, Eroded, Cells, Crags, Mesas, Count };
enum class GenBlend : uint32_t { Add, Max, Min, Replace, Multiply, Count };

struct GenStep {
    GenOp op = GenOp::Noise;
    bool enabled = true;
    std::string name;
    GenBlend blend = GenBlend::Add;
    float amount = 1.0f;  // 0..1: how much of the step's result is taken
    uint32_t seed = 1;
    // Noise.
    GenNoise noise = GenNoise::Fbm;
    float scale = 400.0f;     // feature size, metres
    float height = 120.0f;    // metres from lowest to highest
    float offset = 0.0f;      // shifts the noise up (+) or down (-), share of its height
    uint32_t octaves = 6;
    float lacunarity = 2.0f;  // each octave's frequency over the last
    float gain = 0.5f;        // each octave's amplitude over the last
    float warp = 0.0f;        // domain warp, metres
    float warpScale = 300.0f; // its feature size, metres
    float sharpness = 1.0f;   // ridged: ridge sharpness; cells / mesas: edge sharpness
    // Terrace / plateau.
    float step = 12.0f;       // terrace height, metres
    float stepSharpness = 0.7f;
    float strata = 0.3f;      // noise breaking the terraces up
    float level = 150.0f;     // plateau: world height
    float knee = 20.0f;       // plateau: metres over which it softens
    // Smooth.
    float radius = 4.0f;      // metres
    // Erode.
    uint32_t iterations = 300;
    float cellSize = 2.0f;    // metres per erosion cell (coarser: larger valleys, faster)
    float rain = 0.012f;      // metres of water per second
    float capacity = 1.0f;    // sediment the flow can carry
    float solubility = 0.05f; // how fast it picks ground up
    float deposition = 0.1f;  // how fast it drops it (low: sediment spreads into smooth fans)
    float evaporation = 0.3f; // (rain / evaporation: the water a flat stays under, 3 cm)
    float talus = 38.0f;      // degrees: steeper slopes slump (thermal)
    float thermal = 0.3f;     // its rate (0: none)
    // Paint: a material layer by number, or by its generated look ("grass", "rock", ...;
    // added to the terrain's layers when missing).
    uint32_t layer = 0;
    std::string look;
    // Masks (all steps).
    float heightMin = -1e5f, heightMax = 1e5f, heightSoft = 5.0f;   // world, metres
    float slopeMin = 0.0f, slopeMax = 90.0f, slopeSoft = 4.0f;      // degrees
    float curvatureMin = -1.0f, curvatureMax = 1.0f;                // valleys +, ridges -
    float curvatureScale = 8.0f;                                    // metres
    float maskNoise = 0.0f, maskNoiseScale = 40.0f;                 // 0 off .. 1 full; metres
    float flowMin = 0.0f, flowMax = 1.0f;                           // paint: 0..1 of the flow mask
    float depositMin = 0.0f, depositMax = 1.0f;                     // paint: 0..1 of the deposit mask
};

struct GenRegion {
    bool all = true;                      // the whole terrain, or a square:
    float x = 0.0f, z = 0.0f, size = 512.0f;  // centre and side, metres
    float falloff = 64.0f;                // metres over which it blends into the ground around
    bool flat = false;                    // start from flat ground at `base` (else the current heights)
    float base = 0.0f;
};

struct TerrainGenStack {
    GenRegion region;
    std::vector<GenStep> steps;
};

TerrainGenStack parseGenStack(const std::string& json);
std::string genStackJson(const TerrainGenStack& g);
// The fields `j` has (a step object, or the editor's changes to one) into `s`.
void readGenStep(const Json& j, GenStep& s);
void readGenRegion(const Json& j, GenRegion& r);

extern const char* const kGenOpNames[uint32_t(GenOp::Count)];
extern const char* const kGenNoiseNames[uint32_t(GenNoise::Count)];
extern const char* const kGenBlendNames[uint32_t(GenBlend::Count)];

// Built-in stacks to start from (name, JSON of a TerrainGenStack); layer numbers in their
// paint steps refer to the terrain's layers in order.
struct GenPreset {
    const char* name;
    const char* json;
};
const std::vector<GenPreset>& terrainGeneratorPresets();
// A preset's JSON by name (nullptr: none).
const char* terrainGeneratorPreset(const std::string& name);

class Terrain;
// Paint steps with a look: their layer numbers, adding the looks the terrain lacks to its layers.
void resolveGenLayers(TerrainGenStack& g, Terrain& terrain);

}  // namespace fx
