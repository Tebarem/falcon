#include "vfx/particle_value.h"

#include "core/json.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

float lerp(float a, float b, float t) { return a + (b - a) * t; }
float saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }

const char* evalName(ParticleEval e) { return e == ParticleEval::Life ? "life" : e == ParticleEval::Frame ? "frame" : "seed"; }
ParticleEval evalOf(const std::string& s, ParticleEval fallback)
{
    return s == "life" ? ParticleEval::Life : s == "frame" ? ParticleEval::Frame : s == "seed" ? ParticleEval::Seed : fallback;
}

void readCurve(const Json* j, Curve& c)
{
    c = {};
    if (!j || j->type != Json::Type::Array)
        return;
    for (const Json& k : j->array) {
        if (c.count >= Curve::kMaxKeys || k.type != Json::Type::Array || k.array.size() < 2)
            continue;
        CurveKey& key = c.keys[c.count++];
        key.t = saturate(float(k.array[0].number));
        key.value = float(k.array[1].number);
        key.in = k.array.size() > 2 ? float(k.array[2].number) : 0.0f;
        key.out = k.array.size() > 3 ? float(k.array[3].number) : key.in;
    }
    std::sort(c.keys, c.keys + c.count, [](const CurveKey& a, const CurveKey& b) { return a.t < b.t; });
}

void writeCurve(JsonWriter& w, const char* key, const Curve& c)
{
    w.beginArray(key);
    for (uint32_t i = 0; i < c.count; ++i) {
        const CurveKey& k = c.keys[i];
        w.beginArray().value(nullptr, double(k.t)).value(nullptr, double(k.value));
        if (k.in != 0.0f || k.out != 0.0f)
            w.value(nullptr, double(k.in)).value(nullptr, double(k.out));
        w.endArray();
    }
    w.endArray();
}

bool readColor(const Json* j, float out[4])
{
    if (!j || j->type != Json::Type::Array || j->array.size() < 3)
        return false;
    for (size_t k = 0; k < 4; ++k)
        out[k] = k < j->array.size() ? float(j->array[k].number) : 1.0f;
    return true;
}

void writeColor(JsonWriter& w, const char* key, const float c[4])
{
    w.beginArray(key);
    for (int k = 0; k < 4; ++k)
        w.value(nullptr, double(c[k]));
    w.endArray();
}

void readGradient(const Json* j, Gradient& g)
{
    g = {};
    if (!j || !j->isObject())
        return;
    g.colors = g.alphas = 0;
    if (const Json* c = j->get("colors"); c && c->type == Json::Type::Array)
        for (const Json& k : c->array)
            if (g.colors < Gradient::kMaxKeys && k.type == Json::Type::Array && k.array.size() >= 4)
                g.color[g.colors++] = {saturate(float(k.array[0].number)), float(k.array[1].number), float(k.array[2].number),
                                       float(k.array[3].number)};
    if (const Json* a = j->get("alphas"); a && a->type == Json::Type::Array)
        for (const Json& k : a->array)
            if (g.alphas < Gradient::kMaxKeys && k.type == Json::Type::Array && k.array.size() >= 2)
                g.alpha[g.alphas++] = {saturate(float(k.array[0].number)), float(k.array[1].number)};
    if (!g.colors)
        g.color[g.colors++] = {};
    if (!g.alphas)
        g.alpha[g.alphas++] = {};
    std::sort(g.color, g.color + g.colors, [](const auto& a, const auto& b) { return a.t < b.t; });
    std::sort(g.alpha, g.alpha + g.alphas, [](const auto& a, const auto& b) { return a.t < b.t; });
}

void writeGradient(JsonWriter& w, const char* key, const Gradient& g)
{
    w.beginObject(key).beginArray("colors");
    for (uint32_t i = 0; i < g.colors; ++i)
        w.beginArray().value(nullptr, double(g.color[i].t)).value(nullptr, double(g.color[i].r)).value(nullptr, double(g.color[i].g))
            .value(nullptr, double(g.color[i].b)).endArray();
    w.endArray().beginArray("alphas");
    for (uint32_t i = 0; i < g.alphas; ++i)
        w.beginArray().value(nullptr, double(g.alpha[i].t)).value(nullptr, double(g.alpha[i].a)).endArray();
    w.endArray().endObject();
}

}  // namespace

float Curve::evaluate(float t) const
{
    if (count == 0)
        return 0.0f;
    if (count == 1 || t <= keys[0].t)
        return keys[0].value;
    if (t >= keys[count - 1].t)
        return keys[count - 1].value;
    uint32_t i = 1;
    while (i < count - 1 && keys[i].t < t)
        ++i;
    const CurveKey& k0 = keys[i - 1];
    const CurveKey& k1 = keys[i];
    const float span = k1.t - k0.t;
    if (span <= 1e-6f)
        return k1.value;
    // Hermite, the slopes in value per unit t.
    const float s = (t - k0.t) / span, s2 = s * s, s3 = s2 * s;
    return (2 * s3 - 3 * s2 + 1) * k0.value + (s3 - 2 * s2 + s) * span * k0.out + (-2 * s3 + 3 * s2) * k1.value +
           (s3 - s2) * span * k1.in;
}

Curve Curve::constant(float v)
{
    Curve c;
    c.count = 1;
    c.keys[0] = {0.0f, v, 0.0f, 0.0f};
    return c;
}

Curve Curve::line(float from, float to)
{
    Curve c;
    c.count = 2;
    c.keys[0] = {0.0f, from, to - from, to - from};
    c.keys[1] = {1.0f, to, to - from, to - from};
    return c;
}

ParticleFloat ParticleFloat::range(float lo, float hi)
{
    ParticleFloat f;
    f.mode = Range;
    f.a = lo;
    f.b = hi;
    return f;
}

ParticleFloat ParticleFloat::curve(const Curve& c)
{
    ParticleFloat f;
    f.mode = CurveMode;
    f.eval = ParticleEval::Life;
    f.curveA = c;
    return f;
}

float ParticleFloat::evaluate(float life, float seed, float frame) const
{
    const float d = eval == ParticleEval::Life ? life : eval == ParticleEval::Frame ? frame : seed;
    switch (mode) {
    case Range:
        return lerp(a, b, d);
    case CurveMode:
        return curveA.evaluate(d);
    case CurveRange:
        // The curves along `d`, one picked between them by the particle's seed (s&box's CurveRange).
        return lerp(curveA.evaluate(d), curveB.evaluate(d), eval == ParticleEval::Seed ? particleRandom(uint32_t(seed * 16777216.0f), 77u) : seed);
    default:
        return a;
    }
}

bool ParticleFloat::isNearlyZero() const { return mode == Constant && std::fabs(a) < 1e-6f; }

void Gradient::evaluate(float t, float out[4]) const
{
    // Colour.
    if (colors == 0) {
        out[0] = out[1] = out[2] = 1.0f;
    } else if (colors == 1 || t <= color[0].t) {
        out[0] = color[0].r, out[1] = color[0].g, out[2] = color[0].b;
    } else if (t >= color[colors - 1].t) {
        out[0] = color[colors - 1].r, out[1] = color[colors - 1].g, out[2] = color[colors - 1].b;
    } else {
        uint32_t i = 1;
        while (i < colors - 1 && color[i].t < t)
            ++i;
        const GradientColorKey& k0 = color[i - 1];
        const GradientColorKey& k1 = color[i];
        const float s = k1.t > k0.t ? (t - k0.t) / (k1.t - k0.t) : 1.0f;
        out[0] = lerp(k0.r, k1.r, s), out[1] = lerp(k0.g, k1.g, s), out[2] = lerp(k0.b, k1.b, s);
    }
    // Alpha.
    if (alphas == 0) {
        out[3] = 1.0f;
    } else if (alphas == 1 || t <= alpha[0].t) {
        out[3] = alpha[0].a;
    } else if (t >= alpha[alphas - 1].t) {
        out[3] = alpha[alphas - 1].a;
    } else {
        uint32_t i = 1;
        while (i < alphas - 1 && alpha[i].t < t)
            ++i;
        const GradientAlphaKey& k0 = alpha[i - 1];
        const GradientAlphaKey& k1 = alpha[i];
        const float s = k1.t > k0.t ? (t - k0.t) / (k1.t - k0.t) : 1.0f;
        out[3] = lerp(k0.a, k1.a, s);
    }
}

void ParticleGradient::evaluate(float life, float seed, float frame, float out[4]) const
{
    const float d = eval == ParticleEval::Seed ? seed : eval == ParticleEval::Frame ? frame : life;
    switch (mode) {
    case Range:
        for (int k = 0; k < 4; ++k)
            out[k] = lerp(a[k], b[k], eval == ParticleEval::Life ? seed : d);
        return;
    case GradientMode:
        gradientA.evaluate(d, out);
        return;
    case GradientRange: {
        float o[4];
        gradientA.evaluate(d, out);
        gradientB.evaluate(d, o);
        const float pick = eval == ParticleEval::Seed ? particleRandom(uint32_t(seed * 16777216.0f), 77u) : seed;
        for (int k = 0; k < 4; ++k)
            out[k] = lerp(out[k], o[k], pick);
        return;
    }
    default:
        std::memcpy(out, a, sizeof(a));
    }
}

float particleRandom(uint32_t seed, uint32_t salt)
{
    // PCG-style hash of the two: well spread, no state.
    uint32_t h = seed * 747796405u + salt * 2891336453u + 0x9E3779B9u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    return float(h >> 8) * (1.0f / 16777216.0f);
}

bool readParticleFloat(const Json& j, ParticleFloat& out)
{
    if (j.type == Json::Type::Number) {
        out = ParticleFloat(float(j.number));
        return true;
    }
    if (j.type == Json::Type::Bool) {
        out = ParticleFloat(j.b ? 1.0f : 0.0f);
        return true;
    }
    if (!j.isObject())
        return false;
    out = {};
    const std::string mode = j.getString("mode", "constant");
    out.mode = mode == "range" ? ParticleFloat::Range : mode == "curve" ? ParticleFloat::CurveMode
             : mode == "curveRange" ? ParticleFloat::CurveRange : ParticleFloat::Constant;
    const bool curves = out.mode == ParticleFloat::CurveMode || out.mode == ParticleFloat::CurveRange;
    out.eval = evalOf(j.getString("eval"), curves ? ParticleEval::Life : ParticleEval::Seed);
    out.a = float(j.getNumber("a", 0.0));
    out.b = float(j.getNumber("b", out.a));
    readCurve(j.get("curveA"), out.curveA);
    readCurve(j.get("curveB"), out.curveB);
    return true;
}

void writeParticleFloat(JsonWriter& w, const char* key, const ParticleFloat& v)
{
    if (v.mode == ParticleFloat::Constant) {
        w.value(key, double(v.a));
        return;
    }
    const bool curves = v.mode == ParticleFloat::CurveMode || v.mode == ParticleFloat::CurveRange;
    w.beginObject(key).value("mode", v.mode == ParticleFloat::Range ? "range" : v.mode == ParticleFloat::CurveMode ? "curve" : "curveRange")
        .value("eval", evalName(v.eval));
    if (curves) {
        writeCurve(w, "curveA", v.curveA);
        if (v.mode == ParticleFloat::CurveRange)
            writeCurve(w, "curveB", v.curveB);
    } else {
        w.value("a", double(v.a)).value("b", double(v.b));
    }
    w.endObject();
}

bool readParticleGradient(const Json& j, ParticleGradient& out)
{
    out = {};
    if (readColor(&j, out.a)) {
        std::memcpy(out.b, out.a, sizeof(out.a));
        return true;
    }
    if (!j.isObject())
        return false;
    const std::string mode = j.getString("mode", "constant");
    out.mode = mode == "range" ? ParticleGradient::Range : mode == "gradient" ? ParticleGradient::GradientMode
             : mode == "gradientRange" ? ParticleGradient::GradientRange : ParticleGradient::Constant;
    out.eval = evalOf(j.getString("eval"), ParticleEval::Life);
    readColor(j.get("a"), out.a);
    if (!readColor(j.get("b"), out.b))
        std::memcpy(out.b, out.a, sizeof(out.a));
    readGradient(j.get("gradientA"), out.gradientA);
    readGradient(j.get("gradientB"), out.gradientB);
    return true;
}

void writeParticleGradient(JsonWriter& w, const char* key, const ParticleGradient& v)
{
    if (v.mode == ParticleGradient::Constant) {
        writeColor(w, key, v.a);
        return;
    }
    w.beginObject(key).value("mode", v.mode == ParticleGradient::Range ? "range" : v.mode == ParticleGradient::GradientMode ? "gradient" : "gradientRange")
        .value("eval", evalName(v.eval));
    if (v.mode == ParticleGradient::Range) {
        writeColor(w, "a", v.a);
        writeColor(w, "b", v.b);
    } else {
        writeGradient(w, "gradientA", v.gradientA);
        if (v.mode == ParticleGradient::GradientRange)
            writeGradient(w, "gradientB", v.gradientB);
    }
    w.endObject();
}

bool operator==(const ParticleFloat& a, const ParticleFloat& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }
bool operator==(const ParticleGradient& a, const ParticleGradient& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

}  // namespace fx
