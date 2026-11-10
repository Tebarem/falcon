#include "audio/audio_mixer.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace fx {

namespace {

uint32_t slotOf(AudioVoice v) { return (v & 0xffffu) - 1u; }

// Distance attenuation: 1 within minDistance, then inverse distance (the spatializer's model when
// there is one) faded to 0 over the last tenth of maxDistance, or a straight line to maxDistance.
float attenuation(const Spatializer* spatializer, float distance, const AudioVoiceParams& p)
{
    const float near = std::max(p.minDistance, 1e-3f);
    if (p.flags & kVoiceLinearRolloff)
        return p.maxDistance > near ? std::clamp((p.maxDistance - distance) / (p.maxDistance - near), 0.0f, 1.0f)
                                    : (distance <= near ? 1.0f : 0.0f);
    float g = spatializer ? spatializer->distanceAttenuation(distance, near) : near / std::max(distance, near);
    if (p.maxDistance > near)
        g *= std::clamp((p.maxDistance - distance) / (0.1f * p.maxDistance), 0.0f, 1.0f);
    return g;
}

}  // namespace

void AudioMixer::init(uint32_t maxVoices, Spatializer* spatializer)
{
    maxVoices = std::clamp(maxVoices, 1u, 0xfffeu);
    generation_.assign(maxVoices, 0);
    live_.assign(maxVoices, false);
    free_.clear();
    for (uint32_t i = maxVoices; i-- > 0;)
        free_.push_back(uint16_t(i));
    voices_.assign(maxVoices, Voice{});
    spatializer_ = spatializer;
    freeSpatial_.clear();
    if (spatializer_)
        for (uint32_t i = spatializer_->slots(); i-- > 0;)
            freeSpatial_.push_back(int(i));
}

void AudioMixer::push(const Command& c)
{
    // Keep order: anything waiting goes first.
    while (!overflow_.empty() && commands_.push(overflow_.front()))
        overflow_.erase(overflow_.begin());
    if (!overflow_.empty() || !commands_.push(c))
        overflow_.push_back(c);
}

AudioStream::AudioStream(uint32_t channels, uint32_t capacityFrames) : channels_(channels == 2 ? 2u : 1u), capacity_(64)
{
    while (capacity_ < capacityFrames && capacity_ < (1u << 22))
        capacity_ <<= 1;
    samples_.assign(size_t(capacity_) * channels_, 0.0f);
}

uint32_t AudioStream::write(const float* interleaved, uint32_t frames)
{
    const uint32_t h = head_.load(std::memory_order_relaxed);
    const uint32_t room = capacity_ - (h - tail_.load(std::memory_order_acquire));
    const uint32_t n = std::min(frames, room);
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t c = 0; c < channels_; ++c)
            samples_[size_t((h + i) & (capacity_ - 1)) * channels_ + c] = interleaved[size_t(i) * channels_ + c];
    head_.store(h + n, std::memory_order_release);
    return n;
}

uint32_t AudioStream::queued() const
{
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
}

uint32_t AudioStream::read(float* left, float* right, uint32_t frames)
{
    const uint32_t t = tail_.load(std::memory_order_relaxed);
    const uint32_t n = std::min(frames, head_.load(std::memory_order_acquire) - t);
    for (uint32_t i = 0; i < n; ++i) {
        const float* s = &samples_[size_t((t + i) & (capacity_ - 1)) * channels_];
        left[i] = s[0];
        right[i] = channels_ == 2 ? s[1] : s[0];
    }
    std::fill(left + n, left + frames, 0.0f);
    std::fill(right + n, right + frames, 0.0f);
    tail_.store(t + n, std::memory_order_release);
    if (n < frames && started_)
        underruns_.fetch_add(1, std::memory_order_relaxed);
    started_ |= n > 0;
    return n;
}

AudioVoice AudioMixer::play(const AudioClip* clip, const AudioVoiceParams& params)
{
    if (free_.empty() || (!clip && !params.stream && params.toneHz <= 0.0f) || (clip && clip->frames == 0))
        return 0;
    const uint16_t slot = free_.back();
    free_.pop_back();
    live_[slot] = true;
    ++liveCount_;
    const AudioVoice id = (AudioVoice(generation_[slot]) << 16) | (slot + 1u);
    Command c;
    c.type = Command::Play;
    c.voice = id;
    c.clip = clip;
    c.params = params;
    push(c);
    return id;
}

void AudioMixer::stop(AudioVoice voice, float fadeSeconds)
{
    if (!playing(voice))
        return;
    Command c;
    c.type = Command::Stop;
    c.voice = voice;
    c.a = fadeSeconds;
    push(c);
}

void AudioMixer::set(AudioVoice voice, float gain, float pitch, float3 position)
{
    if (!playing(voice))
        return;
    Command c;
    c.type = Command::Set;
    c.voice = voice;
    c.params.gain = gain;
    c.params.pitch = pitch;
    c.params.position = position;
    push(c);
}

void AudioMixer::setListener(const ListenerPose& listener)
{
    Command c;
    c.type = Command::Listener;
    c.listener = listener;
    push(c);
}

void AudioMixer::setBusGain(AudioBus bus, float gain)
{
    Command c;
    c.type = Command::BusGain;
    c.a = float(bus);
    c.b = gain;
    push(c);
}

void AudioMixer::setSpatializerEnabled(bool on)
{
    Command c;
    c.type = Command::Spatializer;
    c.a = on ? 1.0f : 0.0f;
    push(c);
}

void AudioMixer::setDirect(AudioVoice voice, float occlusion, const float transmission[3])
{
    if (!playing(voice))
        return;
    Command c;
    c.type = Command::Direct;
    c.voice = voice;
    c.direct[0] = occlusion;
    for (int b = 0; b < 3; ++b)
        c.direct[1 + b] = transmission[b];
    push(c);
}

void AudioMixer::setReflections(AudioVoice voice, int slot, void* source)
{
    if (!playing(voice))
        return;
    Command c;
    c.type = Command::Reflections;
    c.voice = voice;
    c.a = float(slot);
    c.source = source;
    push(c);
}

void AudioMixer::setPathing(AudioVoice voice, void* source)
{
    if (!playing(voice))
        return;
    Command c;
    c.type = Command::Pathing;
    c.voice = voice;
    c.source = source;
    push(c);
}

void AudioMixer::setReflectionGain(float gain)
{
    Command c;
    c.type = Command::ReflectionGain;
    c.a = gain;
    push(c);
}

void AudioMixer::setUnderwater(float amount)
{
    Command c;
    c.type = Command::Underwater;
    c.a = amount;
    push(c);
}

void AudioMixer::collect()
{
    AudioVoice v = 0;
    while (finished_.pop(v)) {
        const uint32_t slot = slotOf(v);
        if (slot >= live_.size() || !live_[slot] || generation_[slot] != (v >> 16))
            continue;
        live_[slot] = false;
        --liveCount_;
        ++generation_[slot];
        free_.push_back(uint16_t(slot));
    }
    // Commands that found the ring full go out now that the mixer has drained it.
    while (!overflow_.empty() && commands_.push(overflow_.front()))
        overflow_.erase(overflow_.begin());
}

bool AudioMixer::playing(AudioVoice voice) const
{
    const uint32_t slot = slotOf(voice);
    return voice && slot < live_.size() && live_[slot] && generation_[slot] == (voice >> 16);
}

AudioMixerStats AudioMixer::stats() const
{
    AudioMixerStats s;
    s.blocks = blocks_.load(std::memory_order_relaxed);
    s.mixNs = mixNs_.load(std::memory_order_relaxed);
    s.voices = liveCount_;
    s.spatialized = spatialized_.load(std::memory_order_relaxed);
    s.paths = paths_.load(std::memory_order_relaxed);
    s.peak = peak_.load(std::memory_order_relaxed);
    return s;
}

void AudioMixer::render(float* out, uint32_t frames)
{
    while (frames > 0) {
        if (blockRead_ == kAudioFrameSize) {
            mixBlock();
            blockRead_ = 0;
        }
        const uint32_t n = std::min(frames, kAudioFrameSize - blockRead_);
        std::copy_n(block_ + blockRead_ * 2, n * 2, out);
        out += n * 2;
        frames -= n;
        blockRead_ += n;
    }
}

void AudioMixer::finish(Voice& v)
{
    if (v.spatialSlot >= 0)
        freeSpatial_.push_back(v.spatialSlot);
    const AudioVoice id = v.id;
    v = Voice{};
    // The main thread drains this every frame; a full ring only delays freeing the slot.
    finished_.push(id);
}

uint32_t AudioMixer::source(Voice& v, float* left, float* right, bool& ended)
{
    ended = false;
    const float pitch = std::max(v.p.pitch, 0.0f);
    if (v.p.stream) {  // (X4) a script's: what it wrote, silence after (it plays on until stopped)
        v.p.stream->read(left, right, kAudioFrameSize);
        return kAudioFrameSize;
    }
    if (!v.clip) {  // test tone: phase in turns, wrapped to keep precision
        const double step = double(v.p.toneHz) * double(pitch) / double(kAudioSampleRate);
        for (uint32_t i = 0; i < kAudioFrameSize; ++i) {
            left[i] = right[i] = float(std::sin(2.0 * double(kPi) * v.position));
            v.position += step;
            if (v.position >= 1.0)
                v.position -= 1.0;
        }
        return kAudioFrameSize;
    }
    const AudioClip& c = *v.clip;
    const bool loop = (v.p.flags & kVoiceLoop) != 0;
    const double frames = double(c.frames);
    uint32_t i = 0;
    for (; i < kAudioFrameSize; ++i) {
        if (v.position >= frames) {
            if (!loop) {
                ended = true;
                break;
            }
            v.position = std::fmod(v.position, frames);
        }
        const uint32_t i0 = uint32_t(v.position);
        const uint32_t i1 = i0 + 1 < c.frames ? i0 + 1 : (loop ? 0 : i0);
        const float t = float(v.position - double(i0));
        if (c.channels == 1) {
            const float s = c.samples[i0] + (c.samples[i1] - c.samples[i0]) * t;
            left[i] = right[i] = s;
        } else {
            const float* a = &c.samples[size_t(i0) * 2];
            const float* b = &c.samples[size_t(i1) * 2];
            left[i] = a[0] + (b[0] - a[0]) * t;
            right[i] = a[1] + (b[1] - a[1]) * t;
        }
        v.position += pitch;
    }
    std::fill(left + i, left + kAudioFrameSize, 0.0f);
    std::fill(right + i, right + kAudioFrameSize, 0.0f);
    return i;
}

void AudioMixer::mixBlock()
{
    const auto t0 = std::chrono::steady_clock::now();
    Command c;
    while (commands_.pop(c)) {
        switch (c.type) {
        case Command::Play: {
            Voice& v = voices_[slotOf(c.voice)];
            if (v.id)
                finish(v);
            v = Voice{};
            v.id = c.voice;
            v.clip = c.clip;
            v.p = c.params;
            if ((v.p.flags & kVoiceSpatial) && !freeSpatial_.empty()) {
                v.spatialSlot = freeSpatial_.back();
                freeSpatial_.pop_back();
                spatializer_->reset(uint32_t(v.spatialSlot));
            }
            break;
        }
        case Command::Stop: {
            Voice& v = voices_[slotOf(c.voice)];
            if (v.id == c.voice)
                v.fadeStep = std::max(1e-6f, float(kAudioFrameSize) / (std::max(c.a, 1e-4f) * float(kAudioSampleRate)));
            break;
        }
        case Command::Set: {
            Voice& v = voices_[slotOf(c.voice)];
            if (v.id == c.voice) {
                v.p.gain = c.params.gain;
                v.p.pitch = c.params.pitch;
                v.p.position = c.params.position;
            }
            break;
        }
        case Command::Listener:
            listener_ = c.listener;
            break;
        case Command::BusGain:
            busGain_[size_t(c.a)] = c.b;
            break;
        case Command::Spatializer:
            spatializerOn_ = c.a != 0.0f;
            break;
        case Command::Reflections: {
            Voice& v = voices_[slotOf(c.voice)];
            if (v.id == c.voice) {
                const int slot = c.source ? int(c.a) : -1;
                if (slot >= 0 && slot != v.reflectionSlot)
                    spatializer_->resetReflection(uint32_t(slot));  // (it was another voice's)
                v.reflectionSlot = slot;
                v.reflectionSource = slot >= 0 ? c.source : nullptr;
            }
            break;
        }
        case Command::Pathing: {
            Voice& v = voices_[slotOf(c.voice)];
            if (v.id == c.voice)
                v.pathSource = c.source;
            break;
        }
        case Command::ReflectionGain:
            reflectionGain_ = c.a;
            break;
        case Command::Underwater:
            underwater_ = std::clamp(c.a, 0.0f, 1.0f);
            break;
        case Command::Direct: {
            Voice& v = voices_[slotOf(c.voice)];
            if (v.id == c.voice) {
                std::copy(std::begin(c.direct), std::end(c.direct), v.direct);
                v.hasDirect = true;
            }
            break;
        }
        }
    }

    std::fill(std::begin(mixL_), std::end(mixL_), 0.0f);
    std::fill(std::begin(mixR_), std::end(mixR_), 0.0f);
    std::fill(std::begin(flatL_), std::end(flatL_), 0.0f);
    std::fill(std::begin(flatR_), std::end(flatR_), 0.0f);
    const float inv = 1.0f / float(kAudioFrameSize);
    uint32_t spatialized = 0, paths = 0;
    const bool reflections = spatializer_ && reflectionGain_ > 0.0f && spatializer_->reflectionSlots() > 0;
    if (reflections)
        spatializer_->beginReflections();
    for (Voice& v : voices_) {
        if (!v.id)
            continue;
        bool ended = false;
        source(v, srcL_, srcR_, ended);
        const bool flat = v.p.bus == AudioBus::Music || v.p.bus == AudioBus::Ui;
        float* const toL = flat ? flatL_ : mixL_;
        float* const toR = flat ? flatR_ : mixR_;
        const float fade0 = v.fade;
        const float fade1 = std::max(0.0f, v.fade - v.fadeStep);
        v.fade = fade1;
        float gain1 = v.p.gain * busGain_[size_t(v.p.bus)] * busGain_[size_t(AudioBus::Master)];
        if (v.p.flags & kVoiceSpatial) {
            float3 local = v.p.position;
            if (!(v.p.flags & kVoiceListenerRelative)) {
                const float3 d = v.p.position - listener_.position;
                local = {dot(d, listener_.right), dot(d, listener_.up), -dot(d, listener_.forward)};
            }
            const float distance = length(local);
            gain1 *= attenuation(spatializer_, distance, v.p);
            const bool hrtf = v.spatialSlot >= 0 && spatializerOn_;
            if (v.hasDirect && !hrtf)  // panned: occlusion as a level (what gets through, all bands alike)
                gain1 *= v.direct[0] + (1.0f - v.direct[0]) * (v.direct[1] + v.direct[2] + v.direct[3]) / 3.0f;
            const float3 dir = distance > 1e-4f ? local * (1.0f / distance) : float3{0.0f, 0.0f, -1.0f};
            const float gain0 = v.fresh ? gain1 : v.gain;
            const float dry1 = v.p.gain * busGain_[size_t(v.p.bus)] * busGain_[size_t(AudioBus::Master)];
            const float dry0 = v.fresh ? dry1 : v.dryGain;
            // Mono, with the gain (and any fade) ramped over the block.
            for (uint32_t i = 0; i < kAudioFrameSize; ++i) {
                const float t = float(i) * inv;
                const float fade = fade0 + (fade1 - fade0) * t;
                mono_[i] = 0.5f * (srcL_[i] + srcR_[i]);
                srcL_[i] = mono_[i] * (gain0 + (gain1 - gain0) * t) * fade;
                dry_[i] = mono_[i] * (dry0 + (dry1 - dry0) * t) * fade;
            }
            v.dryGain = dry1;
            // Reflections: its own (the simulated echoes carry the distance), else the listener's
            // reverb hears it as it arrives.
            if (reflections && (v.p.flags & kVoiceReflections)) {
                if (v.reflectionSlot >= 0 && v.reflectionSource)
                    spatializer_->reflect(uint32_t(v.reflectionSlot), v.reflectionSource, dry_);
                else
                    spatializer_->reverbSend(srcL_);
            }
            if (hrtf) {
                const float air = (v.p.flags & kVoiceAirAbsorption) ? distance : -1.0f;
                spatializer_->apply(uint32_t(v.spatialSlot), dir, air, v.hasDirect ? v.direct : nullptr, srcL_, outL_, outR_);
                // Its baked paths around what blocks it: as much as the direct path is blocked (a clear
                // line of sight is a path too). They fall off as 1 / their length from 1 m: scaled
                // like the direct path to its minimum distance, never above it.
                if (v.pathSource && spatializer_->paths()) {
                    const float near = std::min(v.p.minDistance, std::max(distance, 1.0f));
                    const float path1 = v.hasDirect ? std::clamp(1.0f - v.direct[0], 0.0f, 1.0f) * near : 0.0f;
                    const float path0 = v.fresh ? path1 : v.pathGain;
                    if (path0 > 0.0f || path1 > 0.0f) {
                        spatializer_->path(uint32_t(v.spatialSlot), v.pathSource, dry_, path0, path1, listener_, outL_, outR_);
                        ++paths;
                    }
                    v.pathGain = path1;
                }
                for (uint32_t i = 0; i < kAudioFrameSize; ++i) {
                    toL[i] += outL_[i];
                    toR[i] += outR_[i];
                }
                ++spatialized;
            } else {
                const float angle = (std::clamp(dir.x, -1.0f, 1.0f) + 1.0f) * 0.25f * kPi;
                const float panL1 = std::cos(angle), panR1 = std::sin(angle);
                const float panL0 = v.fresh ? panL1 : v.panL, panR0 = v.fresh ? panR1 : v.panR;
                for (uint32_t i = 0; i < kAudioFrameSize; ++i) {
                    const float t = float(i) * inv;
                    toL[i] += srcL_[i] * (panL0 + (panL1 - panL0) * t);
                    toR[i] += srcL_[i] * (panR0 + (panR1 - panR0) * t);
                }
                v.panL = panL1;
                v.panR = panR1;
            }
            v.gain = gain1;
        } else {
            const float gain0 = v.fresh ? gain1 : v.gain;
            for (uint32_t i = 0; i < kAudioFrameSize; ++i) {
                const float t = float(i) * inv;
                const float g = (gain0 + (gain1 - gain0) * t) * (fade0 + (fade1 - fade0) * t);
                toL[i] += srcL_[i] * g;
                toR[i] += srcR_[i] * g;
            }
            v.gain = gain1;
        }
        v.fresh = false;
        if (ended || (v.fadeStep > 0.0f && fade1 <= 0.0f))
            finish(v);
    }
    if (reflections)
        spatializer_->endReflections(listener_, reflectionGain_, mixL_, mixR_);
    // Under water: the world muffled (two one-pole low-passes, the cutoff ramped over the block).
    // Above it the filter is bypassed, its state following the signal (no click coming back).
    if (underwater_ > 0.0f || underwaterLast_ > 0.0f) {
        const auto coefficient = [](float amount) {
            const float hz = 20000.0f * std::pow(350.0f / 20000.0f, amount);
            return 1.0f - std::exp(-2.0f * kPi * std::min(hz, 0.45f * float(kAudioSampleRate)) / float(kAudioSampleRate));
        };
        const float a0 = coefficient(underwaterLast_), a1 = coefficient(underwater_);
        const float g0 = 1.0f - 0.25f * underwaterLast_, g1 = 1.0f - 0.25f * underwater_;
        for (uint32_t i = 0; i < kAudioFrameSize; ++i) {
            const float t = float(i) * inv;
            const float a = a0 + (a1 - a0) * t, g = g0 + (g1 - g0) * t;
            muffle_[0] += a * (mixL_[i] - muffle_[0]);
            muffle_[1] += a * (muffle_[0] - muffle_[1]);
            muffle_[2] += a * (mixR_[i] - muffle_[2]);
            muffle_[3] += a * (muffle_[2] - muffle_[3]);
            mixL_[i] = muffle_[1] * g;
            mixR_[i] = muffle_[3] * g;
        }
        underwaterLast_ = underwater_;
    } else {
        muffle_[0] = muffle_[1] = mixL_[kAudioFrameSize - 1];
        muffle_[2] = muffle_[3] = mixR_[kAudioFrameSize - 1];
    }
    float peak = 0.0f;
    for (uint32_t i = 0; i < kAudioFrameSize; ++i) {
        block_[i * 2] = std::clamp(mixL_[i] + flatL_[i], -1.0f, 1.0f);
        block_[i * 2 + 1] = std::clamp(mixR_[i] + flatR_[i], -1.0f, 1.0f);
        peak = std::max({peak, std::fabs(block_[i * 2]), std::fabs(block_[i * 2 + 1])});
    }
    peak_.store(peak, std::memory_order_relaxed);
    spatialized_.store(spatialized, std::memory_order_relaxed);
    paths_.store(paths, std::memory_order_relaxed);
    blocks_.fetch_add(1, std::memory_order_relaxed);
    mixNs_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()),
                     std::memory_order_relaxed);
}

}  // namespace fx
