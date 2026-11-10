// falcon_wavdiff: checks audio captures (falcon --audio-capture) for the audio tests.
//
//   falcon_wavdiff a.wav b.wav [--exact | --tolerance T]
//       the same length and rate, and no sample further apart than T (--exact: 0)
//   falcon_wavdiff a.wav [--skip S] [--rms LO,HI] [--tone HZ] [--louder left|right RATIO] [--ratio-to b.wav LO,HI]
//                        [--band HZ] (levels: of a sine at HZ alone, not the whole signal's RMS)
//       after skipping S seconds: the RMS of both channels within [LO, HI]; at least 90% of the
//       power in a sine at HZ; one channel's RMS at least RATIO times the other's; each channel's
//       RMS over b.wav's (the same skip) within [LO, HI]
//   falcon_wavdiff --write-tone out.wav HZ RATE CHANNELS SECONDS [DELAY]
//       a 16-bit PCM sine (amplitude 0.5) to play back in tests, after DELAY seconds of silence
//   --until T: checks see only up to T seconds (with --skip: a window)
//
// Reads 16/24/32-bit PCM and 32-bit float WAVs. Exit code 0 when every check passes.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Wav {
    uint32_t rate = 0, channels = 0;
    std::vector<std::vector<float>> data;  // per channel
    size_t frames() const { return data.empty() ? 0 : data[0].size(); }
};

uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

bool readWav(const char* path, Wav& w)
{
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "%s: cannot open\n", path);
        return false;
    }
    std::vector<uint8_t> bytes;
    uint8_t buf[65536];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) || std::memcmp(bytes.data() + 8, "WAVE", 4)) {
        std::fprintf(stderr, "%s: not a WAV file\n", path);
        return false;
    }
    uint16_t format = 0, bits = 0;
    const uint8_t* samples = nullptr;
    size_t sampleBytes = 0;
    for (size_t at = 12; at + 8 <= bytes.size();) {
        const uint8_t* chunk = bytes.data() + at;
        const size_t size = std::min<size_t>(u32(chunk + 4), bytes.size() - at - 8);
        if (!std::memcmp(chunk, "fmt ", 4) && size >= 16) {
            format = u16(chunk + 8);
            w.channels = u16(chunk + 10);
            w.rate = u32(chunk + 12);
            bits = u16(chunk + 22);
            if (format == 0xfffe && size >= 26)  // WAVE_FORMAT_EXTENSIBLE: the sub-format's first word
                format = u16(chunk + 32);
        } else if (!std::memcmp(chunk, "data", 4)) {
            samples = chunk + 8;
            sampleBytes = size;
        }
        at += 8 + size + (size & 1);
    }
    const bool pcm = format == 1 && (bits == 16 || bits == 24 || bits == 32);
    const bool flt = format == 3 && bits == 32;
    if (!samples || !w.channels || !w.rate || (!pcm && !flt)) {
        std::fprintf(stderr, "%s: unsupported WAV (format %u, %u bits)\n", path, format, bits);
        return false;
    }
    const size_t stride = size_t(bits / 8) * w.channels;
    const size_t frames = sampleBytes / stride;
    w.data.assign(w.channels, std::vector<float>(frames));
    for (size_t i = 0; i < frames; ++i)
        for (uint32_t c = 0; c < w.channels; ++c) {
            const uint8_t* p = samples + i * stride + size_t(c) * (bits / 8);
            float v = 0.0f;
            if (flt)
                std::memcpy(&v, p, 4);
            else if (bits == 16)
                v = float(int16_t(u16(p))) / 32768.0f;
            else if (bits == 24)
                v = float(int32_t((uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 24)) >> 8) / 8388608.0f;
            else
                v = float(double(int32_t(u32(p))) / 2147483648.0);
            w.data[c][i] = v;
        }
    return true;
}

bool writeTone(const char* path, double hz, uint32_t rate, uint32_t channels, double seconds, double delay)
{
    std::FILE* f = std::fopen(path, "wb");
    if (!f)
        return false;
    const uint32_t silent = uint32_t(delay * rate);
    const uint32_t frames = silent + uint32_t(seconds * rate);
    const uint32_t data = frames * channels * 2;
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    w32(36 + data);
    std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16);
    w16(1);
    w16(uint16_t(channels));
    w32(rate);
    w32(rate * channels * 2);
    w16(uint16_t(channels * 2));
    w16(16);
    std::fwrite("data", 1, 4, f);
    w32(data);
    for (uint32_t i = 0; i < frames; ++i) {
        const int16_t s = i < silent ? int16_t(0) : int16_t(std::lround(0.5 * std::sin(2.0 * kPi * hz * double(i - silent) / double(rate)) * 32767.0));
        for (uint32_t c = 0; c < channels; ++c)
            w16(uint16_t(s));
    }
    std::fclose(f);
    return true;
}

double rms(const std::vector<float>& x, size_t from)
{
    double sum = 0.0;
    for (size_t i = from; i < x.size(); ++i)
        sum += double(x[i]) * double(x[i]);
    return x.size() > from ? std::sqrt(sum / double(x.size() - from)) : 0.0;
}

// The share of the signal's power in a sine at hz (its projection on sin and cos).
// The RMS of the signal's sine at hz (what is at other frequencies left out).
double toneLevel(const std::vector<float>& x, size_t from, double hz, double rate)
{
    double a = 0.0, b = 0.0;
    const double w = 2.0 * kPi * hz / rate;
    for (size_t i = from; i < x.size(); ++i) {
        a += double(x[i]) * std::cos(w * double(i));
        b += double(x[i]) * std::sin(w * double(i));
    }
    const double n = double(x.size() - from);
    return n > 0.0 ? std::sqrt((a * a + b * b) * 2.0) / n : 0.0;
}

double toneShare(const std::vector<float>& x, size_t from, double hz, double rate)
{
    double a = 0.0, b = 0.0, total = 0.0;
    const double w = 2.0 * kPi * hz / rate;
    for (size_t i = from; i < x.size(); ++i) {
        const double v = x[i];
        a += v * std::cos(w * double(i));
        b += v * std::sin(w * double(i));
        total += v * v;
    }
    const double n = double(x.size() - from);
    if (n <= 0.0 || total <= 0.0)
        return 0.0;
    const double amplitude2 = (a * a + b * b) * 4.0 / (n * n);
    return (amplitude2 * 0.5) / (total / n);
}

int usage()
{
    std::fprintf(stderr,
                 "usage: falcon_wavdiff a.wav b.wav [--exact | --tolerance T]\n"
                 "       falcon_wavdiff a.wav [--skip S] [--rms LO,HI] [--tone HZ] [--louder left|right RATIO] [--ratio-to b.wav LO,HI] [--band HZ]\n"
                 "       falcon_wavdiff --write-tone out.wav HZ RATE CHANNELS SECONDS [DELAY]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && !std::strcmp(argv[1], "--write-tone")) {
        if (argc != 7 && argc != 8)
            return usage();
        const uint32_t channels = uint32_t(std::atoi(argv[5]));
        if (!writeTone(argv[2], std::atof(argv[3]), uint32_t(std::atoi(argv[4])), channels, std::atof(argv[6]), argc == 8 ? std::atof(argv[7]) : 0.0)) {
            std::fprintf(stderr, "%s: cannot write\n", argv[2]);
            return 1;
        }
        return 0;
    }
    std::vector<const char*> files;
    double skip = 0.0, until = 0.0, tolerance = -1.0, rmsLo = -1.0, rmsHi = -1.0, tone = 0.0, louderRatio = 0.0, band = 0.0;
    int louder = -1;  // channel expected louder
    const char* ratioTo = nullptr;
    double ratioLo = 0.0, ratioHi = 0.0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
        if (!std::strcmp(a, "--exact")) {
            tolerance = 0.0;
        } else if (!std::strcmp(a, "--tolerance") && next) {
            tolerance = std::atof(argv[++i]);
        } else if (!std::strcmp(a, "--skip") && next) {
            skip = std::atof(argv[++i]);
        } else if (!std::strcmp(a, "--until") && next) {
            until = std::atof(argv[++i]);
        } else if (!std::strcmp(a, "--rms") && next) {
            if (std::sscanf(argv[++i], "%lf,%lf", &rmsLo, &rmsHi) != 2)
                return usage();
        } else if (!std::strcmp(a, "--tone") && next) {
            tone = std::atof(argv[++i]);
        } else if (!std::strcmp(a, "--band") && next) {
            band = std::atof(argv[++i]);
        } else if (!std::strcmp(a, "--louder") && i + 2 < argc) {
            louder = !std::strcmp(argv[i + 1], "left") ? 0 : !std::strcmp(argv[i + 1], "right") ? 1 : -2;
            louderRatio = std::atof(argv[i + 2]);
            i += 2;
            if (louder < 0)
                return usage();
        } else if (!std::strcmp(a, "--ratio-to") && i + 2 < argc) {
            ratioTo = argv[i + 1];
            if (std::sscanf(argv[i + 2], "%lf,%lf", &ratioLo, &ratioHi) != 2)
                return usage();
            i += 2;
        } else if (a[0] == '-') {
            return usage();
        } else {
            files.push_back(a);
        }
    }
    if (files.empty() || files.size() > 2)
        return usage();

    Wav w;
    if (!readWav(files[0], w))
        return 1;
    bool ok = true;
    if (files.size() == 2) {
        Wav b;
        if (!readWav(files[1], b))
            return 1;
        if (w.rate != b.rate || w.channels != b.channels || w.frames() != b.frames()) {
            std::printf("different shapes: %u Hz x %u, %zu frames vs %u Hz x %u, %zu frames\n", w.rate, w.channels, w.frames(),
                        b.rate, b.channels, b.frames());
            return 1;
        }
        double worst = 0.0;
        size_t differing = 0;
        for (uint32_t c = 0; c < w.channels; ++c)
            for (size_t i = 0; i < w.frames(); ++i) {
                const double d = std::fabs(double(w.data[c][i]) - double(b.data[c][i]));
                worst = std::max(worst, d);
                differing += d > std::max(tolerance, 0.0);
            }
        std::printf("%zu frames, %zu samples differ, largest difference %.9f\n", w.frames(), differing, worst);
        ok = worst <= std::max(tolerance, 0.0);
        return ok ? 0 : 1;
    }

    if (until > 0.0)  // (a window: everything after it is left out)
        for (auto& c : w.data)
            c.resize(std::min(c.size(), size_t(until * w.rate)));
    const size_t from = std::min(w.frames(), size_t(skip * w.rate));
    std::vector<double> levels;
    for (uint32_t c = 0; c < w.channels; ++c)
        levels.push_back(band > 0.0 ? toneLevel(w.data[c], from, band, double(w.rate)) : rms(w.data[c], from));
    std::printf("%s: %u Hz, %u channels, %.3f s; RMS", files[0], w.rate, w.channels, double(w.frames()) / w.rate);
    if (band > 0.0)
        std::printf(" at %g Hz", band);
    for (double l : levels)
        std::printf(" %.5f", l);
    std::printf("\n");
    if (w.frames() <= from) {
        std::printf("FAIL: nothing after --skip\n");
        return 1;
    }
    if (rmsLo >= 0.0)
        for (uint32_t c = 0; c < w.channels; ++c)
            if (levels[c] < rmsLo || levels[c] > rmsHi) {
                std::printf("FAIL: channel %u RMS %.5f outside [%g, %g]\n", c, levels[c], rmsLo, rmsHi);
                ok = false;
            }
    if (tone > 0.0)
        for (uint32_t c = 0; c < w.channels; ++c) {
            const double share = toneShare(w.data[c], from, tone, double(w.rate));
            std::printf("channel %u: %.1f%% of the power at %g Hz\n", c, share * 100.0, tone);
            if (share < 0.9) {
                std::printf("FAIL: channel %u is not a %g Hz tone\n", c, tone);
                ok = false;
            }
        }
    if (ratioTo) {
        Wav b;
        if (!readWav(ratioTo, b))
            return 1;
        for (uint32_t c = 0; c < w.channels && c < b.channels; ++c) {
            const size_t bFrom = std::min(b.frames(), size_t(skip * b.rate));
            const double other = band > 0.0 ? toneLevel(b.data[c], bFrom, band, double(b.rate)) : rms(b.data[c], bFrom);
            const double ratio = levels[c] / std::max(other, 1e-12);
            std::printf("channel %u: %.4f of %s's RMS\n", c, ratio, ratioTo);
            if (ratio < ratioLo || ratio > ratioHi) {
                std::printf("FAIL: channel %u's ratio outside [%g, %g]\n", c, ratioLo, ratioHi);
                ok = false;
            }
        }
    }
    if (louder >= 0) {
        if (w.channels != 2)
            return usage();
        const double ratio = levels[size_t(louder)] / std::max(levels[size_t(1 - louder)], 1e-12);
        std::printf("%s / %s RMS: %.2f\n", louder ? "right" : "left", louder ? "left" : "right", ratio);
        if (ratio < louderRatio) {
            std::printf("FAIL: expected at least %g\n", louderRatio);
            ok = false;
        }
    }
    return ok ? 0 : 1;
}
