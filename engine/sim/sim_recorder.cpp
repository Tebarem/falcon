#include "sim/sim_recorder.h"

#include "core/log.h"
#include "scene/scene.h"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace fx {

namespace {

constexpr char kMagic[8] = {'F', 'X', 'S', 'I', 'M', 'R', 'E', 'C'};
constexpr uint32_t kVersion = 1;

struct Writer {
    std::vector<uint8_t> bytes;
    template <typename T> void put(const T& v)
    {
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        bytes.insert(bytes.end(), p, p + sizeof(T));
    }
};

struct Reader {
    const std::vector<uint8_t>& bytes;
    size_t at = 0;
    bool ok = true;
    template <typename T> T get()
    {
        T v = {};
        if (at + sizeof(T) > bytes.size()) {
            ok = false;
            return v;
        }
        std::memcpy(&v, bytes.data() + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
};

}  // namespace

bool SimRecorder::startRecord(const char* path)
{
    mode_ = Mode::Record;
    path_ = path;
    FX_INFO("sim-record: recording to %s", path);
    return true;
}

bool SimRecorder::startReplay(const char* path)
{
    std::vector<uint8_t> bytes;
    if (FILE* f = std::fopen(path, "rb")) {
        uint8_t chunk[65536];
        size_t n;
        while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
            bytes.insert(bytes.end(), chunk, chunk + n);
        std::fclose(f);
    } else {
        FX_ERROR("sim-replay: cannot open %s", path);
        return false;
    }
    Reader r{bytes};
    char magic[8];
    for (char& c : magic)
        c = r.get<char>();
    const uint32_t version = r.get<uint32_t>();
    const uint32_t keyCount = r.get<uint32_t>();
    const uint32_t frameCount = r.get<uint32_t>();
    if (!r.ok || std::memcmp(magic, kMagic, 8) != 0 || version != kVersion || keyCount > 512) {
        FX_ERROR("sim-replay: %s is not a recording of this version", path);
        return false;
    }
    std::vector<uint8_t> keys(keyCount, 0);
    frames_.resize(frameCount);
    for (Frame& fr : frames_) {
        fr.dt = r.get<float>();
        const uint16_t changed = r.get<uint16_t>();
        for (uint16_t i = 0; i < changed; ++i) {
            const uint16_t key = r.get<uint16_t>();
            const uint8_t value = r.get<uint8_t>();
            if (key < keyCount)
                keys[key] = value;
        }
        fr.keys = keys;
        fr.mouse.dx = r.get<float>(), fr.mouse.dy = r.get<float>(), fr.mouse.wheel = r.get<float>();
        fr.mouse.x = r.get<float>(), fr.mouse.y = r.get<float>();
        fr.mouse.buttons = r.get<uint32_t>(), fr.mouse.buttonsPrev = r.get<uint32_t>();
        const uint8_t padMask = r.get<uint8_t>();
        for (int s = 0; s < 8; ++s)
            if (padMask & (1u << s)) {
                FxGamepad& g = fr.pads[s];
                g.buttons = r.get<uint32_t>(), g.buttonsPrev = r.get<uint32_t>();
                for (float& a : g.axes)
                    a = r.get<float>();
                g.id = r.get<uint32_t>(), g.type = r.get<uint32_t>();
            }
        const uint16_t textLength = r.get<uint16_t>();
        for (uint16_t i = 0; i < textLength; ++i)
            fr.text.push_back(r.get<char>());
        const uint16_t tickCount = r.get<uint16_t>();
        fr.ticks.resize(tickCount);
        for (TickHashes& t : fr.ticks)
            t.tick = r.get<uint64_t>(), t.physics = r.get<uint64_t>(), t.entities = r.get<uint64_t>();
        fr.frameHash = r.get<uint64_t>();
        if (!r.ok) {
            FX_ERROR("sim-replay: %s is cut short", path);
            return false;
        }
    }
    mode_ = Mode::Replay;
    path_ = path;
    FX_INFO("sim-replay: %u frames from %s", frameCount, path);
    return true;
}

uint32_t SimRecorder::random()
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

float SimRecorder::frameDelta(float measured)
{
    float dt = measured;
    tickInFrame_ = 0;
    if (mode_ == Mode::Replay) {
        if (frame_ < frames_.size())
            dt = frames_[frame_].dt;
    } else {
        if (demo_ && !demoRealTime_)
            dt = float(4 + random() % 37) * 0.001f;  // a player's uneven frames: 4 to 40 ms
        if (mode_ == Mode::Record) {
            frames_.emplace_back();
            frame_ = frames_.size() - 1;
            frames_.back().dt = dt;
        }
    }
    clock_ += double(dt);
    return dt;
}

void SimRecorder::demoInput(ScriptHost::FrameInput& in)
{
    static constexpr int32_t kKeys[] = {SDL_SCANCODE_W, SDL_SCANCODE_A,      SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_SPACE,
                                        SDL_SCANCODE_LSHIFT, SDL_SCANCODE_E, SDL_SCANCODE_Q, SDL_SCANCODE_C, SDL_SCANCODE_1};
    if (random() % 12 == 0)
        held_.push_back({kKeys[random() % std::size(kKeys)], int32_t(1 + random() % 90)});
    std::memset(in.keys, 0, size_t(in.keyCount));
    for (Held& h : held_)
        if (h.key < in.keyCount)
            in.keys[h.key] = 1;
    for (Held& h : held_)
        --h.frames;
    std::erase_if(held_, [](const Held& h) { return h.frames <= 0; });
    FxMouse& m = *in.mouse;
    m.dx = float(int32_t(random() % 41) - 20) * 0.5f;
    m.dy = float(int32_t(random() % 21) - 10) * 0.5f;
    m.wheel = random() % 50 == 0 ? (random() & 1 ? 1.0f : -1.0f) : 0.0f;
    m.x = 640.0f, m.y = 360.0f;
    m.buttonsPrev = m.buttons;
    if (random() % 15 == 0)
        demoButtons_ ^= 1u << (random() % 2);  // left or right held / let go
    m.buttons = demoButtons_;
    in.text->clear();
}

void SimRecorder::input(ScriptHost::FrameInput& in)
{
    if (mode_ == Mode::Replay) {
        if (frame_ >= frames_.size())
            return;
        const Frame& fr = frames_[frame_];
        std::memset(in.keys, 0, size_t(in.keyCount));
        std::memcpy(in.keys, fr.keys.data(), std::min(fr.keys.size(), size_t(in.keyCount)));
        FxMouse& m = *in.mouse;
        m.dx = fr.mouse.dx, m.dy = fr.mouse.dy, m.wheel = fr.mouse.wheel;
        m.x = fr.mouse.x, m.y = fr.mouse.y;
        m.buttons = fr.mouse.buttons, m.buttonsPrev = fr.mouse.buttonsPrev;
        for (int s = 0; s < 8; ++s)
            in.gamepads[s] = fr.pads[s];
        *in.text = fr.text;
        return;
    }
    if (demo_)
        demoInput(in);
    if (mode_ == Mode::Record && !frames_.empty()) {
        Frame& fr = frames_.back();
        fr.keys.assign(in.keys, in.keys + in.keyCount);
        fr.mouse = *in.mouse;
        for (int s = 0; s < 8; ++s)
            fr.pads[s] = in.gamepads[s];
        fr.text = *in.text;
    }
}

void SimRecorder::tick(const TickHashes& h)
{
    if (mode_ == Mode::Record) {
        if (!frames_.empty())
            frames_.back().ticks.push_back(h);
        return;
    }
    if (mode_ != Mode::Replay || diverged_ || frame_ >= frames_.size())
        return;
    const Frame& fr = frames_[frame_];
    const size_t i = tickInFrame_++;
    if (i >= fr.ticks.size()) {
        FX_ERROR("sim-replay: frame %zu ran more ticks than recorded (%zu)", frame_, fr.ticks.size());
        diverged_ = true;
        return;
    }
    const TickHashes& want = fr.ticks[i];
    if (want.physics != h.physics || want.entities != h.entities || want.tick != h.tick) {
        FX_ERROR("sim-replay: tick %llu (frame %zu) came out differently:%s%s%s", (unsigned long long)h.tick, frame_,
                 want.tick != h.tick ? " tick number" : "", want.physics != h.physics ? " physics bodies" : "",
                 want.entities != h.entities ? " entity transforms" : "");
        diverged_ = true;
        return;
    }
    ++ticksChecked_;
}

void SimRecorder::frameEnd(uint64_t entities)
{
    if (mode_ == Mode::Record) {
        if (!frames_.empty())
            frames_.back().frameHash = entities;
        return;
    }
    if (mode_ != Mode::Replay || frame_ >= frames_.size())
        return;
    const Frame& fr = frames_[frame_];
    if (!diverged_ && tickInFrame_ != fr.ticks.size()) {
        FX_ERROR("sim-replay: frame %zu ran %zu ticks, the recording %zu", frame_, tickInFrame_, fr.ticks.size());
        diverged_ = true;
    }
    if (!diverged_ && fr.frameHash != entities) {
        FX_ERROR("sim-replay: frame %zu's entity transforms came out differently after the frame (animation, OnUpdate, "
                 "OnLateUpdate: its ticks matched)",
                 frame_);
        diverged_ = true;
    }
    ++frame_;
}

uint64_t SimRecorder::hashEntities(Scene& scene)
{
    uint64_t sum = 0;
    // (not the entities' ids: a rollback makes the entities it spawns again anew)
    scene.world().each([&sum](flecs::entity, const WorldTransform& t) {
        uint64_t h = 1469598103934665603ull;
        for (const float v : t.rows) {
            uint32_t bits;
            std::memcpy(&bits, &v, sizeof(bits));
            h = (h ^ bits) * 1099511628211ull;
        }
        sum += h ^ (h >> 29);
    });
    return sum;
}

bool SimRecorder::finish()
{
    if (mode_ == Mode::Record) {
        Writer w;
        for (char c : kMagic)
            w.put(c);
        const uint32_t keyCount = frames_.empty() ? 0 : uint32_t(frames_.front().keys.size());
        w.put(kVersion);
        w.put(keyCount);
        w.put(uint32_t(frames_.size()));
        std::vector<uint8_t> keys(keyCount, 0);
        uint64_t ticks = 0;
        for (const Frame& fr : frames_) {
            w.put(fr.dt);
            std::vector<uint16_t> changed;
            for (uint32_t k = 0; k < keyCount && k < fr.keys.size(); ++k)
                if (fr.keys[k] != keys[k])
                    changed.push_back(uint16_t(k));
            w.put(uint16_t(changed.size()));
            for (const uint16_t k : changed) {
                w.put(k);
                w.put(fr.keys[k]);
                keys[k] = fr.keys[k];
            }
            w.put(fr.mouse.dx), w.put(fr.mouse.dy), w.put(fr.mouse.wheel), w.put(fr.mouse.x), w.put(fr.mouse.y);
            w.put(fr.mouse.buttons), w.put(fr.mouse.buttonsPrev);
            uint8_t padMask = 0;
            for (int s = 0; s < 8; ++s)
                if (fr.pads[s].id)
                    padMask |= uint8_t(1u << s);
            w.put(padMask);
            for (int s = 0; s < 8; ++s)
                if (padMask & (1u << s)) {
                    const FxGamepad& g = fr.pads[s];
                    w.put(g.buttons), w.put(g.buttonsPrev);
                    for (const float a : g.axes)
                        w.put(a);
                    w.put(g.id), w.put(g.type);
                }
            const size_t textLength = std::min<size_t>(fr.text.size(), 65535);
            w.put(uint16_t(textLength));
            for (size_t i = 0; i < textLength; ++i)
                w.put(fr.text[i]);
            w.put(uint16_t(fr.ticks.size()));
            for (const TickHashes& t : fr.ticks)
                w.put(t.tick), w.put(t.physics), w.put(t.entities);
            w.put(fr.frameHash);
            ticks += fr.ticks.size();
        }
        FILE* f = std::fopen(path_.c_str(), "wb");
        if (!f || std::fwrite(w.bytes.data(), 1, w.bytes.size(), f) != w.bytes.size()) {
            if (f)
                std::fclose(f);
            FX_ERROR("sim-record: cannot write %s", path_.c_str());
            return false;
        }
        std::fclose(f);
        FX_INFO("sim-record: %zu frames, %llu ticks -> %s", frames_.size(), (unsigned long long)ticks, path_.c_str());
        return true;
    }
    if (mode_ == Mode::Replay) {
        if (diverged_)
            return false;
        if (frame_ < frames_.size()) {
            FX_ERROR("sim-replay: stopped after %zu of %zu frames", frame_, frames_.size());
            return false;
        }
        FX_INFO("sim-replay: %zu frames, %llu ticks: identical", frames_.size(), (unsigned long long)ticksChecked_);
    }
    return true;
}

}  // namespace fx
