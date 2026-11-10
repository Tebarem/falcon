#pragma once

// The editor's terrain tools: creating a terrain, sculpting it with brushes, undo, saving.
//
// A brush is a shape and an operation. The shape: radius, a falloff curve (presets or the
// user's own, 9 points from the center out), an optional alpha image (any grayscale picture in
// <scene folder>/brushes/), rotation, and jitter of position, rotation and size per dab. Dabs
// land every `spacing` x radius along the stroke, and keep coming while the mouse holds still.
// Operations: raise / lower, smooth, flatten (to the height under the first dab), set height,
// ramp (from where the stroke starts to where it ends), terrace, noise, sharpen, stamp (the
// alpha image as a height map, once per click), paint and hole. Shift inverts raise / lower; Ctrl smooths.
// Brush settings can be saved as named presets (<scene folder>/brushes.json).
//
// Strokes change the terrain once per frame (all dabs of the frame together); each stroke is
// one undo step holding the tiles it touched, before and after.
//
// Page -> engine (EditorBridge forwards editor.terrain.*):
//   editor.terrain.create {tiles, spacing, heightMin, heightMax, hills, seed}
//   editor.terrain.delete
//   editor.terrain.brush {op, radius, strength, spacing, rotation, jitterPosition, jitterRotation,
//     jitterScale, falloff [9], falloffPreset, alpha, height, terraceStep, terraceSharpness,
//     noiseScale, noiseAmplitude, stampHeight}   (any subset)
//   editor.terrain.preset.save {name}, editor.terrain.preset.load {name}, editor.terrain.preset.delete {name}
//   editor.terrain.settings {pixels}  (patch detail)
//   editor.terrain.import {image, low, high}  (a grayscale picture, 16-bit PNG best, over the whole
//     terrain: black at `low`, white at `high`; the top row along -z; one undo step)
//   editor.terrain.layer.add {generated} (or images: albedo, normal, roughness, ao, height)
//   editor.terrain.layer.set {index, ...TerrainLayer fields}, editor.terrain.layer.remove (the last)
//   editor.terrain.scatter.add {mesh | preset}, editor.terrain.scatter.set {index, ...ScatterLayer fields},
//   editor.terrain.scatter.remove {index}, editor.terrain.scatter.restore {index} (deleted instances back),
//   editor.terrain.scatter.exclude (deletes the picked scatter instance)
//   editor.terrain.generate.preset {name}, editor.terrain.generate.region {...GenRegion fields},
//   editor.terrain.generate.step.add {op}, .step.set {index, ...GenStep fields}, .step.remove {index},
//   .step.move {index, to}, editor.terrain.generate.run, editor.terrain.generate.cancel
// Generation (terrain/terrain_generate.h) runs on the GPU over a few frames; it is one undo step
// (up to kUndoTiles tiles). The erosion brush marks where it goes; on release, hydraulic and
// thermal erosion run under the stroke (strength: how long).
// Paint brushes: paint (the brush's layer), hole (Shift fills) and scatter (a scatter layer's mask;
// Shift erases), with slope, height and noise filters.

#include "core/math.h"
#include "terrain/terrain.h"
#include "terrain/terrain_layers.h"
#include "terrain/terrain_scatter.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fx {

class GpuDrivenRenderer;
class JsonWriter;
class ScenePak;
struct Json;
struct FrameSettings;

enum class BrushOp : uint32_t { Raise, Lower, Smooth, Flatten, SetHeight, Ramp, Terrace, Noise, Sharpen, Stamp, Paint, Hole, Scatter, Erode, Count };

struct BrushSettings {
    static constexpr uint32_t kCurvePoints = 9;
    BrushOp op = BrushOp::Raise;
    float radius = 12.0f;          // metres
    float strength = 0.5f;         // 0..1
    float spacing = 0.15f;         // between dabs, fraction of the radius
    float rotation = 0.0f;         // degrees (alpha images)
    float jitterPosition = 0.0f;   // fraction of the radius
    float jitterRotation = 0.0f;   // degrees
    float jitterScale = 0.0f;      // fraction of the radius
    float falloff[kCurvePoints] = {1.0f, 0.99f, 0.95f, 0.86f, 0.72f, 0.53f, 0.31f, 0.12f, 0.0f};
    std::string falloffPreset = "smooth";  // smooth, linear, sphere, constant, sharp, custom
    std::string alpha;             // file in <scene folder>/brushes/ ("" = none)
    float height = 0.0f;           // set height: target (world)
    float terraceStep = 4.0f;      // metres between terraces
    float terraceSharpness = 0.7f; // 0 soft slopes .. 1 cliffs
    float noiseScale = 8.0f;       // metres per noise feature
    float noiseAmplitude = 1.0f;   // metres per dab at full strength
    float stampHeight = 10.0f;     // metres (white in the alpha image)
    uint32_t layer = 0;            // paint: the material layer
    uint32_t scatterLayer = 0;     // scatter: the scatter layer whose mask is painted (0..6)
    // Paint and hole filters: where the brush takes (soft edges), and a noise mask over it.
    float filterSlope[2] = {0.0f, 90.0f};         // degrees
    float filterHeight[2] = {-1e5f, 1e5f};        // world
    float filterNoise = 0.0f;                     // 0 off .. 1 full
    float filterNoiseScale = 6.0f;                // metres
};

class TerrainEditor {
public:
    struct Init {
        Terrain* terrain = nullptr;
        GpuDrivenRenderer* renderer = nullptr;
        const ScenePak* pak = nullptr;  // mesh names (scatter assets)
        FrameSettings* settings = nullptr;
        std::string scenePath;
    };
    void init(const Init& init);

    bool valid() const { return terrain_ && terrain_->valid(); }
    Terrain* terrain() { return terrain_; }
    const BrushSettings& brush() const { return brush_; }
    // The page's editor.terrain.* messages. `notice`: what to tell the user (may stay empty);
    // `error`: it went wrong.
    void handle(const std::string& type, const Json& m, std::string& notice, bool& error);
    void write(JsonWriter& w);  // "terrain" object of editor.state

    // Scene view: where a camera ray meets the terrain.
    bool hit(float3 origin, float3 dir, float3& p) const;
    void beginStroke(float3 p, bool invert, bool smooth);
    void strokeTo(float3 p);
    // Once per frame: dabs while the mouse holds still, then the frame's dabs into the terrain.
    void tick(float dt);
    // Ends the stroke; its undo step id, or ~0u when nothing changed.
    uint32_t endStroke();
    bool stroking() const { return stroking_; }
    float3 lastStrokeStart() const { return start_; }  // tests: where the last stroke began
    void scaleRadius(float factor);

    // A picked instance slot: a scatter instance becomes the scatter selection (true), anything
    // else clears it (false).
    bool pickScatter(uint32_t slot);
    bool scatterPicked() const { return picked_.valid; }
    // Tests: scatter layers, deleted instances in all, generated cells.
    uint32_t scatterExclusions() const;
    // The share of the terrain where a scatter layer's slope and height rules let it grow
    // (sampled on a 64 x 64 grid; material layers and painted masks not counted).
    float scatterGround(const ScatterLayer& layer) const;

    // A generation run that finished since the last call: its undo step id (~0u: none, or too
    // large to undo).
    uint32_t takeFinishedEdit() { return std::exchange(finishedEdit_, ~0u); }
    bool generating() const;
    // The scene camera (a generation region "at the view" is centred where it looks).
    void setView(float3 position, float3 forward)
    {
        viewPosition_ = position;
        viewForward_ = forward;
    }

    // Undo steps (EditorBridge's stack holds the ids).
    void undo(uint32_t id);
    void redo(uint32_t id);

    bool dirty() const { return deleted_ || (terrain_ && terrain_->valid() && terrain_->dirty()); }
    bool save();
    // Changed since the last state message (brush radius keys, stroke ends): send the state.
    bool takeChanged() { return std::exchange(changed_, false); }

private:
    struct Dab {
        float x, z, radius, rotation;
    };
    struct TileState {
        uint32_t tx, tz;
        TerrainTileData before, after;
    };
    struct Edit {
        uint32_t generation;  // of the terrain it changed (a deleted / recreated terrain ignores it)
        std::vector<TileState> tiles;
    };

    BrushOp effectiveOp() const;
    float falloff(float t) const;  // t = distance / radius in [0, 1]
    float alphaAt(float lx, float lz) const;  // brush-local, in [-1, 1]
    void addDab(float x, float z);
    void applyDabs(const std::vector<Dab>& dabs);
    void applyPaint(const std::vector<Dab>& dabs, TerrainRect r);
    float filterAt(int32_t gx, int32_t gz) const;
    void writeLayer(JsonWriter& w, const TerrainLayer& l) const;
    void readLayer(const Json& m, TerrainLayer& l) const;
    const std::vector<std::string>& images();
    void applyRamp();
    void importHeights(const Json& m, std::string& notice, bool& error);
    void snapshot(TerrainRect r);
    bool loadAlpha(const std::string& name);
    void setPreset(const std::string& name);
    void readBrush(const Json& m);
    void writeBrush(JsonWriter& w, const BrushSettings& b) const;
    void loadPresets();
    bool savePresets() const;
    std::string sceneFolder() const;

    Terrain* terrain_ = nullptr;
    GpuDrivenRenderer* renderer_ = nullptr;
    const ScenePak* pak_ = nullptr;
    FrameSettings* settings_ = nullptr;
    struct ScatterPick {
        bool valid = false;
        uint32_t layer = 0, asset = 0, candidate = 0;
        int32_t cellX = 0, cellZ = 0;
    } picked_;
    std::vector<std::string> meshNames_;
    static constexpr size_t kUndoTiles = 256;  // generation runs over more tiles are not undoable
    uint32_t finishedEdit_ = ~0u;
    float3 viewPosition_ = {0, 0, 0}, viewForward_ = {0, 0, -1};
    bool undoRun_ = false;
    std::vector<Dab> erodeDabs_;  // the erosion brush's stroke
    void startErosionBrush();
    void stopGenerating();  // a run in progress stopped; what it wrote becomes an undo step
    uint32_t pushEdit();  // before_ (and the tiles' current data) as an undo step
    std::string scenePath_;
    BrushSettings brush_;
    std::vector<std::pair<std::string, BrushSettings>> presets_;
    std::vector<float> alpha_;  // the alpha image, grayscale
    uint32_t alphaW_ = 0, alphaH_ = 0;
    std::string alphaLoaded_;

    bool stroking_ = false, invert_ = false, smooth_ = false;
    float3 last_ = {}, start_ = {};
    float travelled_ = 0.0f, still_ = 0.0f, flattenTarget_ = 0.0f;
    uint32_t dabIndex_ = 0;
    std::vector<Dab> pending_;
    std::unordered_map<uint32_t, TerrainTileData> before_;  // tiles touched by this stroke
    std::vector<uint32_t> materials_;
    std::vector<std::string> images_;  // image files under the scene folder (layer sources)
    uint64_t imagesNs_ = 0;
    std::vector<Edit> edits_;
    std::vector<float> heights_, scratch_;
    bool deleted_ = false, changed_ = false, wasBuilding_ = false;
    // Removed scatter layers' paint: their mask bits cleared over the whole terrain, a tile a
    // frame (pinned), before a new layer may take them.
    uint32_t maskClear_ = 0, clearTile_ = 0;
    bool clearPinned_ = false;
    void clearMasks();
    uint32_t generation_ = 1;
};

}  // namespace fx
