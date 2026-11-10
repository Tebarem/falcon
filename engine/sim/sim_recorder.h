#pragma once

// A game's run recorded and replayed to check that the simulation is deterministic.
// --sim-record <file>: each frame's length and the input the game's scripts saw, and after every
// tick (physics step) the simulation's state hashes. --sim-replay <file>: the same frames and
// input fed back; each tick's hashes must come out the same (another thread count, the editor's
// JIT against a shipped NativeAOT game, another PC). --sim-input-demo <seed>: a synthetic player
// (keys, mouse, frame times from 4 to 40 ms) instead of the real input, for tests.

#include "script/script_host.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fx {

class Scene;

class SimRecorder {
public:
    // What a tick's state hashes cover: the physics bodies, and every entity's transform.
    struct TickHashes {
        uint64_t tick = 0;
        uint64_t physics = 0, entities = 0;
    };

    bool startRecord(const char* path);
    bool startReplay(const char* path);
    // `realTime`: the synthetic player's input only, the real frame times (machines in a network
    // session must keep real time to keep their ticks together).
    void setDemo(uint32_t seed, bool realTime = false) { demo_ = true, demoRealTime_ = realTime, rng_ = seed * 2654435761u + 1u; }
    bool active() const { return mode_ != Mode::Off || demo_; }
    bool replaying() const { return mode_ == Mode::Replay; }

    // Each frame, before the game's update: the frame's length (a replay's or the demo's in place
    // of the measured one), and the game's time since its start (their sum).
    float frameDelta(float measured);
    double clock() const { return clock_; }
    // The frame's input (ScriptHost's input hook): recorded, or replaced by the replay's / demo's.
    void input(ScriptHost::FrameInput& in);
    // After each tick: its hashes (recorded, or checked against the recording).
    void tick(const TickHashes& h);
    // After the frame: the entities' hash once animation and the late update ran.
    void frameEnd(uint64_t entities);
    // Every entity's WorldTransform hashed (in no particular order: a sum of each one's hash; ids
    // left out, as a rollback spawns entities again under new ones).
    static uint64_t hashEntities(Scene& scene);
    // Replay: all recorded frames played (the run ends), and whether anything came out differently.
    bool finished() const { return mode_ == Mode::Replay && (diverged_ || frame_ >= frames_.size()); }
    bool diverged() const { return diverged_; }
    // Record: the file written. Replay: the outcome logged. Returns false when it diverged.
    bool finish();

private:
    enum class Mode { Off, Record, Replay };
    struct Frame {
        float dt = 0.0f;
        std::vector<uint8_t> keys;  // keyCount bytes (decoded)
        FxMouse mouse = {};
        FxGamepad pads[8] = {};
        std::string text;
        std::vector<TickHashes> ticks;
        uint64_t frameHash = 0;
    };
    void demoInput(ScriptHost::FrameInput& in);
    uint32_t random();

    Mode mode_ = Mode::Off;
    bool demo_ = false, demoRealTime_ = false;
    std::string path_;
    std::vector<Frame> frames_;
    size_t frame_ = 0;       // the frame being played (replay) or recorded
    size_t tickInFrame_ = 0;  // replay: this frame's ticks checked
    double clock_ = 0.0;
    uint64_t ticksChecked_ = 0;
    bool diverged_ = false;
    std::vector<uint8_t> lastKeys_;
    uint32_t rng_ = 1;
    // The demo player's held keys and how long each stays (frames).
    struct Held {
        int32_t key;
        int32_t frames;
    };
    std::vector<Held> held_;
    uint32_t demoButtons_ = 0;
};

}  // namespace fx
