#include "audio/steam_audio.h"

#include "core/log.h"
#include "core/platform.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#if FALCON_STEAM_AUDIO
#include <phonon.h>
#endif

namespace fx {

#if FALCON_STEAM_AUDIO

namespace {

// <exe dir>/audio/phonon.dll, loaded by full path (its own imports resolve next to it) before the
// first call through the delay-load import finds it by name.
bool loadPhonon()
{
    static void* module = nullptr;
    if (module)
        return true;
    const std::string file = platform::libraryFileName("phonon");
    const std::filesystem::path path = platform::executableDir() / "audio" / file;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        FX_WARN("audio: Steam Audio missing (bin/audio/%s; scripts/fetch-steamaudio.ps1): panning instead of HRTF", file.c_str());
        return false;
    }
    module = platform::loadLibrary(path);
    if (!module) {
        FX_WARN("audio: %s did not load (%s): panning instead of HRTF", file.c_str(), platform::lastError().c_str());
        return false;
    }
    return true;
}

void IPLCALL logMessage(IPLLogLevel level, const char* message)
{
    if (level == IPL_LOGLEVEL_ERROR)
        FX_ERROR("Steam Audio: %s", message);
    else if (level == IPL_LOGLEVEL_WARNING)
        FX_WARN("Steam Audio: %s", message);
}

}  // namespace

struct SteamAudio::Impl {
    IPLContext context = nullptr;
    IPLHRTF hrtf = nullptr;
    std::vector<IPLBinauralEffect> binaural;
    std::vector<IPLDirectEffect> direct;
    std::vector<float> air;  // a block after air absorption
    std::string version;
    // Reflections.
    Reflections reflections;
    IPLReflectionEffectType type = IPL_REFLECTIONEFFECTTYPE_HYBRID;
    int channels = 4, irSize = 0;
    std::vector<IPLReflectionEffect> reflection;  // per slot
    std::vector<IPLPathEffect> paths;             // per spatial slot (baked pathing), straight to the ears
    IPLReflectionEffect listener = nullptr;        // the listener's reverb
    IPLReflectionMixer mixer = nullptr;            // (convolution)
    IPLAmbisonicsDecodeEffect decode = nullptr;
    std::atomic<void*> listenerSource{nullptr};
    std::vector<float> field, part, send, stereo;  // ambisonic sum, one effect's output, the reverb send, decoded
    bool sent = false;
    std::vector<float*> fieldPtrs, partPtrs;
    float* stereoPtrs[2] = {};

    // One effect's reflections of `in` for a source's simulated echoes: into the mixer
    // (convolution) or added to the field.
    void apply(IPLReflectionEffect effect, IPLSource source, float* in)
    {
        IPLSimulationOutputs out{};
        iplSourceGetOutputs(source, IPL_SIMULATIONFLAGS_REFLECTIONS, &out);
        IPLReflectionEffectParams p = out.reflections;
        p.type = type;
        p.numChannels = channels;
        p.irSize = irSize;
        if (type != IPL_REFLECTIONEFFECTTYPE_PARAMETRIC && !p.ir)
            return;  // not simulated yet
        float* ins[1] = {in};
        IPLAudioBuffer inBuffer{1, int(kAudioFrameSize), ins};
        IPLAudioBuffer partBuffer{channels, int(kAudioFrameSize), partPtrs.data()};
        iplReflectionEffectApply(effect, &p, &inBuffer, &partBuffer, mixer);
        if (!mixer) {
            IPLAudioBuffer fieldBuffer{channels, int(kAudioFrameSize), fieldPtrs.data()};
            iplAudioBufferMix(context, &partBuffer, &fieldBuffer);
        }
    }
};

SteamAudio::SteamAudio() : impl_(std::make_unique<Impl>()) {}
SteamAudio::~SteamAudio() { shutdown(); }

bool SteamAudio::init(uint32_t slots, const Reflections& reflections)
{
    shutdown();
    impl_->reflections = reflections;
    if (!loadPhonon())
        return false;
    Impl& m = *impl_;
    IPLContextSettings cs{};
    cs.version = STEAMAUDIO_VERSION;
    cs.logCallback = logMessage;
    cs.simdLevel = IPL_SIMDLEVEL_AVX2;  // AVX-512 can throttle the clock for little gain here
    if (iplContextCreate(&cs, &m.context) != IPL_STATUS_SUCCESS) {
        FX_WARN("audio: Steam Audio %d.%d.%d did not start (an older phonon.dll?): panning instead of HRTF",
                STEAMAUDIO_VERSION_MAJOR, STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH);
        m.context = nullptr;
        return false;
    }
    IPLAudioSettings as{int(kAudioSampleRate), int(kAudioFrameSize)};
    IPLHRTFSettings hs{};
    hs.type = IPL_HRTFTYPE_DEFAULT;
    hs.volume = 1.0f;
    hs.normType = IPL_HRTFNORMTYPE_NONE;
    if (iplHRTFCreate(m.context, &as, &hs, &m.hrtf) != IPL_STATUS_SUCCESS) {
        FX_WARN("audio: Steam Audio's HRTF did not load: panning instead");
        shutdown();
        return false;
    }
    IPLBinauralEffectSettings bs{m.hrtf};
    IPLDirectEffectSettings ds{1};
    m.binaural.assign(slots, nullptr);
    m.direct.assign(slots, nullptr);
    m.air.assign(kAudioFrameSize, 0.0f);
    for (uint32_t i = 0; i < slots; ++i)
        if (iplBinauralEffectCreate(m.context, &as, &bs, &m.binaural[i]) != IPL_STATUS_SUCCESS ||
            iplDirectEffectCreate(m.context, &as, &ds, &m.direct[i]) != IPL_STATUS_SUCCESS) {
            FX_WARN("audio: Steam Audio's effects did not start: panning instead");
            shutdown();
            return false;
        }
    // Reflections: the effects, the mixer (convolution), the decode. Sized for the longest tail.
    if (reflections.slots > 0) {
        m.type = reflections.type == 0 ? IPL_REFLECTIONEFFECTTYPE_CONVOLUTION
                 : reflections.type == 1 ? IPL_REFLECTIONEFFECTTYPE_PARAMETRIC
                                         : IPL_REFLECTIONEFFECTTYPE_HYBRID;
        m.channels = int((reflections.order + 1) * (reflections.order + 1));
        m.irSize = int(std::ceil(reflections.seconds * float(kAudioSampleRate)));
        IPLReflectionEffectSettings rs{m.type, m.irSize, m.channels};
        m.reflection.assign(reflections.slots, nullptr);
        bool ok = iplReflectionEffectCreate(m.context, &as, &rs, &m.listener) == IPL_STATUS_SUCCESS;
        for (IPLReflectionEffect& e : m.reflection)
            ok = ok && iplReflectionEffectCreate(m.context, &as, &rs, &e) == IPL_STATUS_SUCCESS;
        if (ok && m.type == IPL_REFLECTIONEFFECTTYPE_CONVOLUTION)
            ok = iplReflectionMixerCreate(m.context, &as, &rs, &m.mixer) == IPL_STATUS_SUCCESS;
        IPLAmbisonicsDecodeEffectSettings ds2{};
        ds2.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
        ds2.hrtf = m.hrtf;
        ds2.maxOrder = int(reflections.order);
        ok = ok && iplAmbisonicsDecodeEffectCreate(m.context, &as, &ds2, &m.decode) == IPL_STATUS_SUCCESS;
        if (!ok) {
            FX_WARN("audio: Steam Audio's reflection effects did not start: no reflections");
            m.reflection.clear();
        } else {
            IPLPathEffectSettings ps{};
            ps.maxOrder = int(reflections.order);
            ps.spatialize = IPL_TRUE;
            ps.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
            ps.hrtf = m.hrtf;
            m.paths.assign(slots, nullptr);
            for (IPLPathEffect& e : m.paths)
                if (iplPathEffectCreate(m.context, &as, &ps, &e) != IPL_STATUS_SUCCESS) {
                    FX_WARN("audio: Steam Audio's path effects did not start: no pathing");
                    for (IPLPathEffect& p : m.paths)
                        if (p)
                            iplPathEffectRelease(&p);
                    m.paths.clear();
                    break;
                }
        }
        m.field.assign(size_t(m.channels) * kAudioFrameSize, 0.0f);
        m.part.assign(size_t(m.channels) * kAudioFrameSize, 0.0f);
        m.send.assign(kAudioFrameSize, 0.0f);
        m.stereo.assign(2 * kAudioFrameSize, 0.0f);
        m.fieldPtrs.clear();
        m.partPtrs.clear();
        for (int c = 0; c < m.channels; ++c) {
            m.fieldPtrs.push_back(m.field.data() + size_t(c) * kAudioFrameSize);
            m.partPtrs.push_back(m.part.data() + size_t(c) * kAudioFrameSize);
        }
        m.stereoPtrs[0] = m.stereo.data();
        m.stereoPtrs[1] = m.stereo.data() + kAudioFrameSize;
    }
    m.version = std::to_string(STEAMAUDIO_VERSION_MAJOR) + "." + std::to_string(STEAMAUDIO_VERSION_MINOR) + "." +
                std::to_string(STEAMAUDIO_VERSION_PATCH);
    return true;
}

void SteamAudio::shutdown()
{
    Impl& m = *impl_;
    for (IPLBinauralEffect& e : m.binaural)
        if (e)
            iplBinauralEffectRelease(&e);
    for (IPLDirectEffect& e : m.direct)
        if (e)
            iplDirectEffectRelease(&e);
    for (IPLReflectionEffect& e : m.reflection)
        if (e)
            iplReflectionEffectRelease(&e);
    m.reflection.clear();
    for (IPLPathEffect& e : m.paths)
        if (e)
            iplPathEffectRelease(&e);
    m.paths.clear();
    if (m.listener)
        iplReflectionEffectRelease(&m.listener);
    if (m.mixer)
        iplReflectionMixerRelease(&m.mixer);
    if (m.decode)
        iplAmbisonicsDecodeEffectRelease(&m.decode);
    m.listener = nullptr;
    m.mixer = nullptr;
    m.decode = nullptr;
    m.listenerSource = nullptr;
    m.binaural.clear();
    m.direct.clear();
    if (m.hrtf)
        iplHRTFRelease(&m.hrtf);
    if (m.context)
        iplContextRelease(&m.context);
    m.hrtf = nullptr;
    m.context = nullptr;
    m.version.clear();
}

bool SteamAudio::active() const { return impl_->context != nullptr; }
const char* SteamAudio::version() const { return impl_->version.c_str(); }
uint32_t SteamAudio::slots() const { return uint32_t(impl_->binaural.size()); }

void SteamAudio::reset(uint32_t slot)
{
    iplBinauralEffectReset(impl_->binaural[slot]);
    iplDirectEffectReset(impl_->direct[slot]);
    if (slot < impl_->paths.size())
        iplPathEffectReset(impl_->paths[slot]);
}

bool SteamAudio::paths() const { return !impl_->paths.empty(); }

void SteamAudio::path(uint32_t slot, void* source, const float* dry, float gain0, float gain1, const ListenerPose& listener, float* left,
                      float* right)
{
    Impl& m = *impl_;
    if (slot >= m.paths.size())
        return;
    IPLSimulationOutputs out{};
    iplSourceGetOutputs(static_cast<IPLSource>(source), IPL_SIMULATIONFLAGS_PATHING, &out);
    IPLPathEffectParams p = out.pathing;
    if (!p.shCoeffs)
        return;  // not simulated yet
    p.order = int(m.reflections.order);
    p.binaural = IPL_TRUE;
    p.hrtf = m.hrtf;
    p.listener = {{listener.right.x, listener.right.y, listener.right.z}, {listener.up.x, listener.up.y, listener.up.z},
                  {listener.forward.x, listener.forward.y, listener.forward.z}, {listener.position.x, listener.position.y, listener.position.z}};
    p.normalizeEQ = IPL_FALSE;
    float* in[1] = {const_cast<float*>(dry)};
    IPLAudioBuffer inBuffer{1, int(kAudioFrameSize), in};
    IPLAudioBuffer outBuffer{2, int(kAudioFrameSize), m.stereoPtrs};
    iplPathEffectApply(m.paths[slot], &p, &inBuffer, &outBuffer);
    const float inv = 1.0f / float(kAudioFrameSize);
    for (uint32_t i = 0; i < kAudioFrameSize; ++i) {
        const float g = gain0 + (gain1 - gain0) * float(i) * inv;
        left[i] += m.stereoPtrs[0][i] * g;
        right[i] += m.stereoPtrs[1][i] * g;
    }
}

float SteamAudio::distanceAttenuation(float distance, float minDistance) const
{
    IPLDistanceAttenuationModel model{};
    model.type = IPL_DISTANCEATTENUATIONTYPE_INVERSEDISTANCE;
    model.minDistance = minDistance;
    const IPLVector3 source{0.0f, 0.0f, -distance}, listener{0.0f, 0.0f, 0.0f};
    return iplDistanceAttenuationCalculate(impl_->context, source, listener, &model) * minDistance;
}

void* SteamAudio::context() const { return impl_->context; }

void SteamAudio::setListenerSource(void* source) { impl_->listenerSource = source; }
const SteamAudio::Reflections& SteamAudio::reflections() const { return impl_->reflections; }
uint32_t SteamAudio::reflectionSlots() const { return uint32_t(impl_->reflection.size()); }
void SteamAudio::resetReflection(uint32_t slot) { iplReflectionEffectReset(impl_->reflection[slot]); }

void SteamAudio::beginReflections()
{
    Impl& m = *impl_;
    std::fill(m.field.begin(), m.field.end(), 0.0f);
    std::fill(m.send.begin(), m.send.end(), 0.0f);
    m.sent = false;
}

void SteamAudio::reflect(uint32_t slot, void* source, const float* dry)
{
    Impl& m = *impl_;
    m.apply(m.reflection[slot], static_cast<IPLSource>(source), const_cast<float*>(dry));
}

void SteamAudio::reverbSend(const float* in)
{
    Impl& m = *impl_;
    for (uint32_t i = 0; i < kAudioFrameSize; ++i)
        m.send[i] += in[i];
    m.sent = true;
}

void SteamAudio::endReflections(const ListenerPose& listener, float gain, float* left, float* right)
{
    Impl& m = *impl_;
    // The listener's reverb (its effect runs every block, so its tail rings out after a sound stops).
    if (void* source = m.listenerSource.load())
        m.apply(m.listener, static_cast<IPLSource>(source), m.send.data());
    IPLAudioBuffer fieldBuffer{m.channels, int(kAudioFrameSize), m.fieldPtrs.data()};
    if (m.mixer) {
        IPLReflectionEffectParams p{};
        p.type = m.type;
        p.numChannels = m.channels;
        p.irSize = m.irSize;
        IPLAudioBuffer partBuffer{m.channels, int(kAudioFrameSize), m.partPtrs.data()};
        iplReflectionMixerApply(m.mixer, &p, &partBuffer);
        iplAudioBufferMix(m.context, &partBuffer, &fieldBuffer);
    }
    IPLAmbisonicsDecodeEffectParams dp{};
    dp.order = int(m.reflections.order);
    dp.hrtf = m.hrtf;
    dp.orientation = {{listener.right.x, listener.right.y, listener.right.z}, {listener.up.x, listener.up.y, listener.up.z},
                      {listener.forward.x, listener.forward.y, listener.forward.z}, {listener.position.x, listener.position.y, listener.position.z}};
    dp.binaural = IPL_TRUE;
    IPLAudioBuffer stereoBuffer{2, int(kAudioFrameSize), m.stereoPtrs};
    iplAmbisonicsDecodeEffectApply(m.decode, &dp, &fieldBuffer, &stereoBuffer);
    for (uint32_t i = 0; i < kAudioFrameSize; ++i) {
        left[i] += m.stereoPtrs[0][i] * gain;
        right[i] += m.stereoPtrs[1][i] * gain;
    }
}

void SteamAudio::apply(uint32_t slot, float3 direction, float airDistance, const float* direct, const float* mono, float* left, float* right)
{
    Impl& m = *impl_;
    float* in[1] = {const_cast<float*>(mono)};
    if (airDistance >= 0.0f || direct) {
        IPLDirectEffectParams dp{};
        if (airDistance >= 0.0f) {
            IPLAirAbsorptionModel model{};
            model.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
            dp.flags = IPLDirectEffectFlags(dp.flags | IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION);
            iplAirAbsorptionCalculate(m.context, IPLVector3{0.0f, 0.0f, -airDistance}, IPLVector3{0.0f, 0.0f, 0.0f}, &model, dp.airAbsorption);
        }
        if (direct) {  // occlusion, and what gets through, per band
            dp.flags = IPLDirectEffectFlags(dp.flags | IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION | IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
            dp.transmissionType = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
            dp.occlusion = direct[0];
            for (int b = 0; b < 3; ++b)
                dp.transmission[b] = direct[1 + b];
        }
        float* air[1] = {m.air.data()};
        IPLAudioBuffer dryBuffer{1, int(kAudioFrameSize), in};
        IPLAudioBuffer airBuffer{1, int(kAudioFrameSize), air};
        iplDirectEffectApply(m.direct[slot], &dp, &dryBuffer, &airBuffer);
        in[0] = m.air.data();
    }
    float* out[2] = {left, right};
    IPLAudioBuffer inBuffer{1, int(kAudioFrameSize), in};
    IPLAudioBuffer outBuffer{2, int(kAudioFrameSize), out};
    IPLBinauralEffectParams p{};
    p.direction = {direction.x, direction.y, direction.z};
    p.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
    p.spatialBlend = 1.0f;
    p.hrtf = m.hrtf;
    iplBinauralEffectApply(m.binaural[slot], &p, &inBuffer, &outBuffer);
}

#else  // no Steam Audio in this build

struct SteamAudio::Impl {};
SteamAudio::SteamAudio() : impl_(std::make_unique<Impl>()) {}
SteamAudio::~SteamAudio() = default;
bool SteamAudio::init(uint32_t, const Reflections&)
{
    FX_WARN("audio: built without Steam Audio (scripts/fetch-steamaudio.ps1, then configure): panning instead of HRTF");
    return false;
}
void SteamAudio::shutdown() {}
bool SteamAudio::active() const { return false; }
const char* SteamAudio::version() const { return ""; }
uint32_t SteamAudio::slots() const { return 0; }
void SteamAudio::reset(uint32_t) {}
float SteamAudio::distanceAttenuation(float distance, float minDistance) const { return minDistance / std::max(distance, minDistance); }
void SteamAudio::apply(uint32_t, float3, float, const float*, const float*, float*, float*) {}
void* SteamAudio::context() const { return nullptr; }
void SteamAudio::setListenerSource(void*) {}
const SteamAudio::Reflections& SteamAudio::reflections() const
{
    static const Reflections none{};
    return none;
}
uint32_t SteamAudio::reflectionSlots() const { return 0; }
void SteamAudio::resetReflection(uint32_t) {}
void SteamAudio::beginReflections() {}
void SteamAudio::reflect(uint32_t, void*, const float*) {}
void SteamAudio::reverbSend(const float*) {}
void SteamAudio::endReflections(const ListenerPose&, float, float*, float*) {}
bool SteamAudio::paths() const { return false; }
void SteamAudio::path(uint32_t, void*, const float*, float, float, const ListenerPose&, float*, float*) {}

#endif

}  // namespace fx
