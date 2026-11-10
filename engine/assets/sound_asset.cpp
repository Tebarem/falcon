#include "assets/sound_asset.h"

#include "core/json.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <unordered_map>

namespace fx {

const char* const kSoundBuses[5] = {"sfx", "physics", "ambience", "music", "ui"};

namespace {

const char* const kRolloffs[] = {"inverse", "linear"};
const char* const kSelections[] = {"random", "shuffle", "sequence"};

using K = SoundField::Kind;
#define FX_SF(key, kind, lo, hi, step, group, label, tip) \
    SoundField{#key, kind, offsetof(SoundParams, key), lo, hi, step, group, label, tip}

const std::vector<SoundField> kFields = {
    FX_SF(volume, K::Number, -60, 12, 0.1f, "Playback", "Volume (dB)", "0: the clips as recorded"),
    FX_SF(volumeRandom, K::Number, 0, 24, 0.1f, "Playback", "Vary volume (dB)", "Up to this much louder or quieter each time"),
    FX_SF(pitch, K::Number, -24, 24, 0.1f, "Playback", "Pitch (semitones)", "12: an octave up (and twice as fast)"),
    FX_SF(pitchRandom, K::Number, 0, 12, 0.1f, "Playback", "Vary pitch", "Up to this many semitones higher or lower each time"),
    FX_SF(vary, K::Bool, 0, 1, 1, "Playback", "Vary",
          "Each play a little different: these amounts, or without any the project's Sound variation (one-shots, not music). Off: exactly as recorded"),
    FX_SF(loop, K::Bool, 0, 1, 1, "Playback", "Loop", "Plays until stopped (ambience, engines, music)"),
    SoundField{"selection", K::Choice, offsetof(SoundParams, selection), 0, 2, 1, "Playback", "Clip order",
               "Several clips: one at random, shuffled (none twice until all have played) or in order", kSelections, 3},
    FX_SF(spatial, K::Bool, 0, 1, 1, "Space", "3D", "Placed in the world: heard from where it is, quieter far away (else 2D)"),
    FX_SF(minDistance, K::Number, 0.01f, 1000, 0.05f, "Space", "Min distance (m)", "Full volume within it"),
    FX_SF(maxDistance, K::Number, 0.1f, 10000, 0.5f, "Space", "Max distance (m)", "Silent beyond it"),
    SoundField{"rolloff", K::Choice, offsetof(SoundParams, rolloff), 0, 1, 1, "Space", "Rolloff",
               "Inverse distance (as sound fades in the open: half as loud at twice the distance) or a straight line to the max", kRolloffs, 2},
    FX_SF(airAbsorption, K::Bool, 0, 1, 1, "Space", "Air absorption", "Far away, the highs fade first (Steam Audio's model of air)"),
    FX_SF(occlusion, K::Bool, 0, 1, 1, "Space", "Occlusion", "Muffled behind walls and what else stands between it and the listener"),
    FX_SF(transmission, K::Bool, 0, 1, 1, "Space", "Transmission",
          "Heard through what is in the way, as its acoustic materials let it (else only around it)"),
    FX_SF(reflections, K::Bool, 0, 1, 1, "Space", "Reflections", "Echoes and reverb from the scene around it (Steam Audio)"),
    SoundField{"bus", K::Choice, offsetof(SoundParams, bus), 0, 4, 1, "Mixing", "Bus", "The volume group it plays in (Audio settings)",
               kSoundBuses, 5},
    FX_SF(maxInstances, K::Number, 0, 64, 1, "Mixing", "Max at once", "Playing at the same time (0: any number; the oldest stops)"),
    FX_SF(cooldown, K::Number, 0, 5000, 1, "Mixing", "Cooldown (ms)", "Asked again sooner, it doesn't play (0: always)"),
};
#undef FX_SF

float* number(SoundParams& s, const SoundField& f) { return reinterpret_cast<float*>(reinterpret_cast<char*>(&s) + f.offset); }
float numberOf(const SoundParams& s, const SoundField& f) { return *reinterpret_cast<const float*>(reinterpret_cast<const char*>(&s) + f.offset); }
bool* flag(SoundParams& s, const SoundField& f) { return reinterpret_cast<bool*>(reinterpret_cast<char*>(&s) + f.offset); }
bool flagOf(const SoundParams& s, const SoundField& f) { return *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(&s) + f.offset); }
uint32_t* choice(SoundParams& s, const SoundField& f) { return reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(&s) + f.offset); }
uint32_t choiceOf(const SoundParams& s, const SoundField& f)
{
    return *reinterpret_cast<const uint32_t*>(reinterpret_cast<const char*>(&s) + f.offset);
}

struct Preset {
    const char* name;
    const char* display;
    const char* json;
};

const Preset kPresets[] = {
    {"sound", "Sound", R"({"name": "Sound"})"},
    {"click", "UI click", R"({"name": "UI click", "spatial": false, "bus": "ui", "volume": -6, "pitchRandom": 0.5, "cooldown": 30})"},
    {"footstep", "Footstep", R"({"name": "Footstep", "volumeRandom": 2, "pitchRandom": 1.5, "selection": "shuffle",
        "minDistance": 1, "maxDistance": 30, "maxInstances": 4})"},
    {"ambience", "Ambience loop", R"({"name": "Ambience loop", "loop": true, "bus": "ambience", "minDistance": 5,
        "maxDistance": 80})"},
    {"impact", "Impact", R"({"name": "Impact", "bus": "physics", "volumeRandom": 1.5, "pitchRandom": 1, "maxInstances": 8,
        "cooldown": 20, "maxDistance": 60})"},
    {"music", "Music", R"({"name": "Music", "loop": true, "spatial": false, "bus": "music", "selection": "sequence", "vary": false})"},
};

std::unordered_map<std::string, uint32_t>& idsByPath()
{
    static std::unordered_map<std::string, uint32_t> ids;
    return ids;
}
std::vector<std::string>& pathsById()
{
    static std::vector<std::string> paths(1);  // id 0: none
    return paths;
}

bool endsWith(const std::string& s, const char* suffix)
{
    const size_t n = std::char_traits<char>::length(suffix);
    if (s.size() < n)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != suffix[i])
            return false;
    return true;
}

}  // namespace

const std::vector<SoundField>& soundFields() { return kFields; }

bool soundPreset(const std::string& name, SoundAsset& out)
{
    for (const Preset& p : kPresets)
        if (name == p.name) {
            out = {};
            readSoundFields(Json::parse(p.json), out);
            out.preset = p.name;
            return true;
        }
    return false;
}

const std::vector<std::pair<std::string, std::string>>& soundPresets()
{
    static const std::vector<std::pair<std::string, std::string>> names = [] {
        std::vector<std::pair<std::string, std::string>> v;
        for (const Preset& p : kPresets)
            v.emplace_back(p.name, p.display);
        return v;
    }();
    return names;
}

void readSoundFields(const Json& j, SoundAsset& s)
{
    if (!j.isObject())
        return;
    if (const Json* p = j.get("preset"); p && p->type == Json::Type::String && p->str != s.preset) {
        const std::string name = s.name;
        std::vector<std::string> clips = s.clips;
        if (soundPreset(p->str, s)) {
            s.clips = std::move(clips);
            if (!j.get("name") && !name.empty())
                s.name = name;
        }
    }
    if (const Json* n = j.get("name"); n && n->type == Json::Type::String)
        s.name = n->str;
    if (const Json* c = j.get("clips"); c && c->type == Json::Type::Array) {
        s.clips.clear();
        for (const Json& clip : c->array)
            if (clip.type == Json::Type::String && !clip.str.empty())
                s.clips.push_back(clip.str);
    }
    for (const SoundField& f : kFields) {
        const Json* v = j.get(f.key);
        if (!v)
            continue;
        switch (f.kind) {
        case K::Number:
            if (v->type == Json::Type::Number && std::isfinite(v->number))
                *number(s, f) = std::clamp(float(v->number), f.min, f.max);
            break;
        case K::Bool:
            if (v->type == Json::Type::Bool)
                *flag(s, f) = v->b;
            break;
        case K::Choice:
            for (uint32_t c = 0; c < f.choiceCount; ++c)
                if ((v->type == Json::Type::String && v->str == f.choices[c]) ||
                    (v->type == Json::Type::Number && uint32_t(v->number) == c))
                    *choice(s, f) = c;
            break;
        }
    }
    s.maxDistance = std::max(s.maxDistance, s.minDistance);
}

void writeSoundAsset(JsonWriter& w, const SoundAsset& s)
{
    w.value("format", 1u).value("name", s.name);
    if (!s.preset.empty())
        w.value("preset", s.preset);
    w.beginArray("clips");
    for (const std::string& c : s.clips)
        w.value(nullptr, c);
    w.endArray();
    for (const SoundField& f : kFields)
        switch (f.kind) {
        case K::Number: w.value(f.key, double(numberOf(s, f))); break;
        case K::Bool: w.value(f.key, flagOf(s, f)); break;
        case K::Choice: w.value(f.key, f.choices[std::min(choiceOf(s, f), f.choiceCount - 1)]); break;
        }
}

bool readSoundAsset(const std::string& path, SoundAsset& out, std::string& error)
{
    size_t size = 0;
    void* text = SDL_LoadFile(path.c_str(), &size);
    if (!text) {
        error = "Cannot read " + path;
        return false;
    }
    try {
        const Json j = Json::parse(std::string_view(static_cast<const char*>(text), size));
        SDL_free(text);
        out = {};
        out.name = std::filesystem::path(path).stem().string();
        readSoundFields(j, out);
    } catch (const std::exception& e) {
        error = path + " is malformed (" + e.what() + ")";
        return false;
    }
    return true;
}

bool writeSoundAsset(const std::string& path, const SoundAsset& s, std::string& error)
{
    JsonWriter w;
    w.beginObject();
    writeSoundAsset(w, s);
    w.endObject();
    const std::string text = Json::parse(w.str()).dump(2) + "\n";
    const std::string tmp = path + ".tmp";
    if (!SDL_SaveFile(tmp.c_str(), text.data(), text.size())) {
        error = "Cannot write " + path;
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        error = "Cannot write " + path + " (" + ec.message() + ")";
        return false;
    }
    return true;
}

void writeSoundSchema(JsonWriter& w, const char* key)
{
    w.beginArray(key);
    for (const SoundField& f : kFields) {
        static const char* const kKinds[] = {"number", "bool", "choice"};
        w.beginObject().value("key", f.key).value("kind", kKinds[uint32_t(f.kind)]).value("group", f.group).value("label", f.label);
        if (f.tip && *f.tip)
            w.value("tip", f.tip);
        if (f.kind == K::Number)
            w.value("min", double(f.min)).value("max", double(f.max)).value("step", double(f.step));
        if (f.kind == K::Choice) {
            w.beginArray("choices");
            for (uint32_t c = 0; c < f.choiceCount; ++c)
                w.value(nullptr, f.choices[c]);
            w.endArray();
        }
        w.endObject();
    }
    w.endArray();
}

bool isSoundAssetPath(const std::string& path) { return endsWith(path, ".fsound"); }
bool isAudioClipPath(const std::string& path) { return endsWith(path, ".wav") || endsWith(path, ".ogg"); }

uint32_t soundId(const std::string& path)
{
    if (path.empty())
        return 0;
    auto [it, added] = idsByPath().try_emplace(path, uint32_t(pathsById().size()));
    if (added)
        pathsById().push_back(path);
    return it->second;
}

const std::string& soundPath(uint32_t id)
{
    static const std::string none;
    return id < pathsById().size() ? pathsById()[id] : none;
}

}  // namespace fx
