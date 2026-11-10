#include "water/water_system.h"

#include "core/dmath.h"
#include "core/json.h"
#include "core/log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace fx {

namespace {

constexpr float kGravity = 9.81f;

const char* const kShapeNames[] = {"rect", "circle", "polygon", "ocean", "river", "box"};

// Gerstner sum at a still-surface point: displacement and the normal there (the shaders' gerstner()).
void gerstner(const WaterSystem::Body& b, float x, float z, float t, float3& offset, float3& normal)
{
    offset = {0.0f, 0.0f, 0.0f};
    normal = {0.0f, 1.0f, 0.0f};
    for (uint32_t i = 0; i < b.waveCount; ++i) {
        const GerstnerWave& w = b.waves[i];
        const float theta = w.k * (w.dirX * x + w.dirZ * z) - w.omega * t + w.phase;
        const float c = dmath::cos(theta), s = dmath::sin(theta);
        offset.x += w.steepness * w.amplitude * w.dirX * c;
        offset.y += w.amplitude * s;
        offset.z += w.steepness * w.amplitude * w.dirZ * c;
        const float ka = w.k * w.amplitude;
        normal.x -= w.dirX * ka * c;
        normal.z -= w.dirZ * ka * c;
        normal.y -= w.steepness * ka * s;
    }
    normal = normalize(normal);
}

// Deterministic pseudo-random numbers in [0, 1) (waves must not change between runs).
float hash01(uint32_t seed, uint32_t i)
{
    uint32_t h = seed * 0x9e3779b9u ^ (i + 1) * 0x85ebca6bu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return float(h & 0xffffff) / float(0x1000000);
}

bool insidePolygon(const std::vector<float>& pts, float x, float z)
{
    bool in = false;
    const size_t n = pts.size() / 2;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const float xi = pts[i * 2], zi = pts[i * 2 + 1], xj = pts[j * 2], zj = pts[j * 2 + 1];
        if ((zi > z) != (zj > z) && x < (xj - xi) * (z - zi) / (zj - zi) + xi)
            in = !in;
    }
    return in;
}

}  // namespace

const char* waterShapeName(WaterShape s) { return kShapeNames[std::min(uint32_t(s), uint32_t(WaterShape::Count) - 1)]; }

WaterShape waterShapeFromName(std::string_view name)
{
    for (uint32_t s = 0; s < uint32_t(WaterShape::Count); ++s)
        if (name == kShapeNames[s])
            return WaterShape(s);
    return WaterShape::Rect;
}

void readWaterBodyJson(const Json& j, WaterBodyDesc& d)
{
    if (!j.isObject())
        return;
    if (const Json* s = j.get("shape"); s && s->type == Json::Type::String)
        d.shape = waterShapeFromName(s->str);
    d.width = std::clamp(float(j.getNumber("width", d.width)), 0.1f, 100000.0f);
    d.length = std::clamp(float(j.getNumber("length", d.length)), 0.1f, 100000.0f);
    d.depth = std::clamp(float(j.getNumber("depth", d.depth)), 0.0f, 10000.0f);
    if (const Json* p = j.get("points"); p && p->type == Json::Type::Array) {
        d.points.clear();
        for (const Json& v : p->array)
            if (v.type == Json::Type::Number)
                d.points.push_back(float(v.number));
        if (d.points.size() % 2)
            d.points.pop_back();
    }
    if (const Json* h = j.get("heights"); h && h->type == Json::Type::Array) {
        d.heights.clear();
        for (const Json& v : h->array)
            if (v.type == Json::Type::Number)
                d.heights.push_back(float(v.number));
    }
    if (const Json* m = j.get("material"); m && m->type == Json::Type::String && !m->str.empty())
        d.material = m->str;
    if (const Json* o = j.get("overrides"); o && o->isObject()) {
        JsonWriter w;
        w.beginObject();
        for (const auto& [key, value] : o->object)
            w.raw(key.c_str(), value.dump(0));
        w.endObject();
        d.overrides = w.str();
    }
    d.buoyancy = j.getBool("buoyancy", d.buoyancy);
}

void writeWaterBodyJson(JsonWriter& w, const char* key, const WaterBodyDesc& d)
{
    w.beginObject(key).value("shape", waterShapeName(d.shape)).value("width", double(d.width)).value("length", double(d.length))
        .value("depth", double(d.depth)).value("material", d.material).value("buoyancy", d.buoyancy);
    if (!d.points.empty()) {
        w.beginArray("points");
        for (const float v : d.points)
            w.value(nullptr, double(v));
        w.endArray();
    }
    if (!d.heights.empty()) {
        w.beginArray("heights");
        for (const float v : d.heights)
            w.value(nullptr, double(v));
        w.endArray();
    }
    w.raw("overrides", d.overrides.empty() ? std::string_view("{}") : std::string_view(d.overrides));
    w.endObject();
}

float2 waterBodyToWorld(const WaterSystem::Body& b, float2 local)
{
    const float c = dmath::cos(b.yaw), s = dmath::sin(b.yaw);
    return {b.position.x + local.x * c + local.y * s, b.position.z - local.x * s + local.y * c};
}

float2 waterWorldToBody(const WaterSystem::Body& b, float x, float z)
{
    const float c = dmath::cos(b.yaw), s = dmath::sin(b.yaw);
    const float dx = x - b.position.x, dz = z - b.position.z;
    return {dx * c - dz * s, dx * s + dz * c};
}

void WaterSystem::clear()
{
    bodies_.clear();
    files_.clear();
    ++revision_;
}

uint32_t WaterSystem::add(const WaterBodyDesc& desc, uint64_t entity)
{
    uint32_t id = 0;
    while (id < bodies_.size() && bodies_[id].alive)
        ++id;
    if (id >= kMaxBodies) {
        FX_WARN("water: at most %u bodies", kMaxBodies);
        return ~0u;
    }
    if (id == bodies_.size())
        bodies_.emplace_back();
    Body& b = bodies_[id];
    b = {};
    b.alive = true;
    b.entity = entity;
    b.desc = desc;
    resolve(b);
    updateOcean();
    ++revision_;
    return id;
}

void WaterSystem::set(uint32_t id, const WaterBodyDesc& desc)
{
    if (id >= bodies_.size() || !bodies_[id].alive)
        return;
    bodies_[id].desc = desc;
    resolve(bodies_[id]);
    updateOcean();
    ++revision_;
}

void WaterSystem::remove(uint32_t id)
{
    if (id >= bodies_.size() || !bodies_[id].alive)
        return;
    bodies_[id] = {};
    while (!bodies_.empty() && !bodies_.back().alive)
        bodies_.pop_back();
    updateOcean();
    ++revision_;
}

void WaterSystem::setPose(uint32_t id, float3 position, float yaw)
{
    if (id >= bodies_.size() || !bodies_[id].alive)
        return;
    Body& b = bodies_[id];
    if (b.position.x == position.x && b.position.y == position.y && b.position.z == position.z && b.yaw == yaw)
        return;
    b.position = position;
    b.yaw = yaw;
    ++revision_;
}

void WaterSystem::setVisible(uint32_t id, bool visible)
{
    if (id < bodies_.size() && bodies_[id].alive && bodies_[id].visible != visible) {
        bodies_[id].visible = visible;
        ++revision_;
    }
}

void WaterSystem::setHidden(uint32_t id, bool hidden)
{
    if (id < bodies_.size() && bodies_[id].alive)
        bodies_[id].hidden = hidden;
}

uint32_t WaterSystem::count() const
{
    uint32_t n = 0;
    for (const Body& b : bodies_)
        n += b.alive;
    return n;
}

int32_t WaterSystem::find(uint64_t entity) const
{
    for (size_t i = 0; i < bodies_.size(); ++i)
        if (bodies_[i].alive && bodies_[i].entity == entity)
            return int32_t(i);
    return -1;
}

std::string WaterSystem::resolvePath(const std::string& path) const
{
    std::filesystem::path p(path);
    if (p.is_relative() && !projectDir_.empty())
        p = std::filesystem::path(projectDir_) / p;
    return p.lexically_normal().generic_string();
}

WaterMaterial WaterSystem::baseMaterial(const WaterBodyDesc& desc) const
{
    WaterMaterial m;
    if (desc.material.starts_with("builtin:")) {
        if (!waterPreset(desc.material.substr(8), m))
            waterPreset("lake", m);
        return m;
    }
    const std::string path = resolvePath(desc.material);
    if (auto it = files_.find(path); it != files_.end())
        return it->second;
    std::string error;
    if (!readWaterAsset(path, m, error)) {
        FX_WARN("water: %s (using the Lake preset)", error.c_str());
        waterPreset("lake", m);
    }
    files_[path] = m;
    return m;
}

void WaterSystem::resolve(Body& b)
{
    b.material = baseMaterial(b.desc);
    if (!b.desc.overrides.empty() && b.desc.overrides != "{}") {
        try {
            const std::string name = b.material.name, preset = b.material.preset;
            readWaterFields(Json::parse(b.desc.overrides), b.material);
            b.material.name = name;
            b.material.preset = preset;
        } catch (const std::exception& e) {
            FX_WARN("water: overrides are malformed (%s)", e.what());
        }
    }
    buildWaves(b);
    buildPath(b);
}

void WaterSystem::buildPath(Body& b)
{
    b.path.clear();
    const std::vector<float>& p = b.desc.points;
    const size_t n = p.size() / 2;
    if (b.desc.shape != WaterShape::River || n < 2)
        return;
    auto at = [&](int64_t i) {
        i = std::clamp<int64_t>(i, 0, int64_t(n) - 1);
        const float y = size_t(i) < b.desc.heights.size() ? b.desc.heights[size_t(i)] : 0.0f;
        return float3{p[size_t(i) * 2], y, p[size_t(i) * 2 + 1]};
    };
    constexpr int kSteps = 8;
    float s = 0.0f;
    for (size_t i = 0; i + 1 < n; ++i)
        for (int k = 0; k < (i + 2 == n ? kSteps + 1 : kSteps); ++k) {
            const float t = float(k) / float(kSteps), t2 = t * t, t3 = t2 * t;
            const float3 p0 = at(int64_t(i) - 1), p1 = at(int64_t(i)), p2 = at(int64_t(i) + 1), p3 = at(int64_t(i) + 2);
            // Catmull-Rom (uniform): through every point, smooth between them.
            const float3 q = (p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 +
                              (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) * 0.5f;
            if (!b.path.empty()) {
                const Body::PathPoint& last = b.path.back();
                s += std::sqrt((q.x - last.x) * (q.x - last.x) + (q.z - last.z) * (q.z - last.z));
            }
            b.path.push_back({q.x, q.z, q.y, s});
        }
}

bool WaterSystem::nearestOnPath(const Body& b, float x, float z, uint32_t& segment, float& t, float& d2) const
{
    d2 = 1e30f;
    for (uint32_t i = 0; i + 1 < b.path.size(); ++i) {
        const Body::PathPoint &a = b.path[i], &c = b.path[i + 1];
        const float ex = c.x - a.x, ez = c.z - a.z, len2 = std::max(ex * ex + ez * ez, 1e-12f);
        const float u = std::clamp(((x - a.x) * ex + (z - a.z) * ez) / len2, 0.0f, 1.0f);
        const float dx = a.x + ex * u - x, dz = a.z + ez * u - z, dd = dx * dx + dz * dz;
        if (dd < d2) {
            d2 = dd;
            segment = i;
            t = u;
        }
    }
    return d2 < 1e30f;
}

bool WaterSystem::reloadMaterial(const std::string& path)
{
    const std::string resolved = resolvePath(path);
    files_.erase(resolved);
    bool any = false;
    for (Body& b : bodies_)
        if (b.alive && !b.desc.material.starts_with("builtin:") && resolvePath(b.desc.material) == resolved) {
            resolve(b);
            any = true;
        }
    if (any) {
        updateOcean();
        ++revision_;
    }
    return any;
}

void WaterSystem::splash(float x, float z, float radius, float strength) const
{
    if (splashes_.size() < 1024)
        splashes_.push_back({x, z, radius, strength});
}

std::vector<WaterSystem::Splash> WaterSystem::takeSplashes() const
{
    std::vector<Splash> out;
    out.swap(splashes_);
    return out;
}

void WaterSystem::setFftSize(uint32_t n)
{
    n = std::clamp(n, 64u, 256u);  // (water_fft.slang kMaxN)
    if (n == fftSize_)
        return;
    fftSize_ = n;
    oceanKey_.clear();
    updateOcean();
}

// The spectrum follows the first live body with ocean waves; rebuilt only when what it is made
// from changes (wind, height, choppiness, spread, size), not when other fields do.
void WaterSystem::updateOcean()
{
    oceanBody_ = -1;
    for (size_t i = 0; i < bodies_.size() && oceanBody_ < 0; ++i)
        if (bodies_[i].alive && usesOcean(bodies_[i]))
            oceanBody_ = int32_t(i);
    if (oceanBody_ < 0)
        return;
    const WaterMaterial& m = bodies_[size_t(oceanBody_)].material;
    char key[192];
    std::snprintf(key, sizeof(key), "%g %g %g %g %g %u", double(m.windSpeed), double(m.windDirection), double(m.waveHeight),
                  double(m.choppiness), double(m.waveSpread), fftSize_);
    if (oceanKey_ == key)
        return;
    oceanKey_ = key;
    ocean_.build(m, fftSize_, 1);
}

// The wind's waves (Pierson-Moskowitz): peak frequency 0.877 g / U, significant height
// 0.21 U^2 / g. Eight waves from 1.6x down to a quarter of the peak wavelength, of equal
// steepness, around the wind's direction; their crests add up to half the significant height.
void WaterSystem::buildWaves(Body& b)
{
    const WaterMaterial& m = b.material;
    b.waveCount = 0;
    b.waveAmplitude = 0.0f;
    if (m.waves != WaveMode::Gerstner || m.windSpeed <= 0.0f || m.waveHeight <= 0.0f)
        return;
    const float wind = std::max(m.windSpeed, 0.5f);
    const float wp = 0.877f * kGravity / wind;
    const float peak = 2.0f * kPi * kGravity / (wp * wp);
    const float crest = 0.5f * 0.21f * wind * wind / kGravity * m.waveHeight;
    const float windAngle = m.windDirection * kPi / 180.0f;
    const uint32_t n = kMaxWaves;
    float sum = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        GerstnerWave& w = b.waves[i];
        const float t = float(i) / float(n - 1);
        const float wavelength = peak * (1.6f * dmath::pow(0.25f / 1.6f, t));
        w.k = 2.0f * kPi / wavelength;
        const float angle = windAngle + (hash01(17, i) * 2.0f - 1.0f) * m.waveSpread * kPi * 0.5f;
        w.dirX = dmath::cos(angle);
        w.dirZ = dmath::sin(angle);
        w.amplitude = wavelength;  // equal steepness: scaled below
        sum += wavelength;
        // Deep water: omega^2 = g k, rounded to a multiple of the wrap's frequency.
        const float w0 = float(2.0 * 3.14159265358979 / kTimeWrap);
        w.omega = std::max(1.0f, std::round(std::sqrt(kGravity * w.k) / w0)) * w0;
        w.phase = hash01(29, i) * 2.0f * kPi;
    }
    for (uint32_t i = 0; i < n; ++i) {
        GerstnerWave& w = b.waves[i];
        w.amplitude *= crest / sum;
        // Choppiness 1: the sharpest crests that never loop (sum of Q k A = 1).
        w.steepness = w.amplitude > 0.0f ? std::min(m.choppiness, 1.0f) / (w.k * w.amplitude * float(n)) : 0.0f;
        b.waveAmplitude += w.amplitude;
    }
    b.waveCount = n;
}

void WaterSystem::setTime(double seconds)
{
    time_ = std::fmod(seconds, kTimeWrap);
    if (time_ < 0.0)
        time_ += kTimeWrap;
}

void WaterSystem::setSimTime(double seconds)
{
    simTime_ = std::fmod(seconds, kTimeWrap);
    if (simTime_ < 0.0)
        simTime_ += kTimeWrap;
}

bool WaterSystem::inside(const Body& b, float x, float z) const
{
    if (b.desc.shape == WaterShape::Ocean)
        return true;
    const float2 l = waterWorldToBody(b, x, z);
    switch (b.desc.shape) {
    case WaterShape::Rect:
    case WaterShape::Box: return std::fabs(l.x) <= b.desc.width * 0.5f && std::fabs(l.y) <= b.desc.length * 0.5f;
    case WaterShape::Circle: return l.x * l.x + l.y * l.y <= b.desc.width * b.desc.width * 0.25f;
    case WaterShape::Polygon: return b.desc.points.size() >= 6 && insidePolygon(b.desc.points, l.x, l.y);
    case WaterShape::River: {
        uint32_t segment;
        float t, d2;
        return nearestOnPath(b, l.x, l.y, segment, t, d2) && d2 <= b.desc.width * b.desc.width * 0.25f;
    }
    default: return false;
    }
}

float3 WaterSystem::nearestSurface(const Body& b, float3 p) const
{
    float2 l = waterWorldToBody(b, p.x, p.z);
    float y = b.position.y;
    switch (b.desc.shape) {
    case WaterShape::Ocean: return {p.x, y, p.z};
    case WaterShape::Rect:
    case WaterShape::Box:
        l.x = std::clamp(l.x, -b.desc.width * 0.5f, b.desc.width * 0.5f);
        l.y = std::clamp(l.y, -b.desc.length * 0.5f, b.desc.length * 0.5f);
        break;
    case WaterShape::Circle:
        if (const float r = b.desc.width * 0.5f, d = std::sqrt(l.x * l.x + l.y * l.y); d > r)
            l = {l.x * r / d, l.y * r / d};
        break;
    case WaterShape::Polygon: {
        const std::vector<float>& pts = b.desc.points;
        if (pts.size() < 6 || insidePolygon(pts, l.x, l.y))
            break;
        float best = 1e30f;
        float2 at = l;
        for (size_t i = 0, n = pts.size() / 2; i < n; ++i) {  // the nearest point of its edges
            const float ax = pts[i * 2], az = pts[i * 2 + 1], bx = pts[(i + 1) % n * 2], bz = pts[(i + 1) % n * 2 + 1];
            const float ex = bx - ax, ez = bz - az;
            const float u = std::clamp(((l.x - ax) * ex + (l.y - az) * ez) / std::max(ex * ex + ez * ez, 1e-12f), 0.0f, 1.0f);
            const float qx = ax + ex * u, qz = az + ez * u, d2 = (qx - l.x) * (qx - l.x) + (qz - l.y) * (qz - l.y);
            if (d2 < best) {
                best = d2;
                at = {qx, qz};
            }
        }
        l = at;
        break;
    }
    case WaterShape::River: {
        uint32_t segment;
        float t, d2;
        if (!nearestOnPath(b, l.x, l.y, segment, t, d2))
            break;
        const Body::PathPoint &a = b.path[segment], &c = b.path[segment + 1];
        const float cx = a.x + (c.x - a.x) * t, cz = a.z + (c.z - a.z) * t, half = b.desc.width * 0.5f;
        if (const float d = std::sqrt(d2); d > half)
            l = {cx + (l.x - cx) * half / d, cz + (l.y - cz) * half / d};
        y += a.y + (c.y - a.y) * t;
        break;
    }
    default: break;
    }
    const float2 w = waterBodyToWorld(b, l);
    return {w.x, y, w.y};
}

WaterSample WaterSystem::sample(float3 p) const
{
    WaterSample s;
    const Body* hit = nullptr;
    int32_t id = -1;
    for (size_t i = 0; i < bodies_.size(); ++i) {
        const Body& b = bodies_[i];
        if (!b.alive || !b.visible || !inside(b, p.x, p.z))
            continue;
        // Bodies inside others (a pool in the ocean): the smallest wins; the ocean always last.
        if (!hit || (hit->desc.shape == WaterShape::Ocean && b.desc.shape != WaterShape::Ocean)) {
            hit = &b;
            id = int32_t(i);
        }
    }
    if (!hit)
        return s;
    // The still-surface point the waves move onto (x, z): two fixed-point steps.
    const float t = float(simTime_);
    float3 offset, normal;
    float sx = p.x, sz = p.z;
    for (int it = 0; it < 2; ++it) {
        gerstner(*hit, sx, sz, t, offset, normal);
        sx = p.x - offset.x;
        sz = p.z - offset.z;
    }
    gerstner(*hit, sx, sz, t, offset, normal);
    s.body = id;
    s.height = hit->position.y + offset.y;
    s.normal = normal;
    if (usesOcean(*hit) && ocean_.valid()) {
        float h;
        ocean_.sample(p.x, p.z, t, h, s.normal);
        s.height = hit->position.y + h;
    }
    const float flowAngle = hit->material.windDirection * kPi / 180.0f;
    s.flow = {dmath::cos(flowAngle) * hit->material.flowSpeed, 0.0f, dmath::sin(flowAngle) * hit->material.flowSpeed};
    if (hit->desc.shape == WaterShape::River) {
        // Down the river: its height along the path, its flow along the path's direction (fastest
        // in the middle, slower toward the banks).
        const float2 l = waterWorldToBody(*hit, p.x, p.z);
        uint32_t i;
        float t, d2;
        if (nearestOnPath(*hit, l.x, l.y, i, t, d2)) {
            const Body::PathPoint &a = hit->path[i], &c = hit->path[i + 1];
            s.height += a.y + (c.y - a.y) * t;
            const float2 dirLocal = {c.x - a.x, c.z - a.z};
            const float len = std::max(std::sqrt(dirLocal.x * dirLocal.x + dirLocal.y * dirLocal.y), 1e-6f);
            const float2 w0 = waterBodyToWorld(*hit, {0.0f, 0.0f}), w1 = waterBodyToWorld(*hit, {dirLocal.x / len, dirLocal.y / len});
            const float across = std::sqrt(d2) / std::max(hit->desc.width * 0.5f, 1e-3f);
            const float speed = hit->material.flowSpeed * (1.0f - 0.6f * across * across);
            s.flow = {(w1.x - w0.x) * speed, 0.0f, (w1.y - w0.y) * speed};
        }
    }
    s.depth = s.height - p.y;
    return s;
}

}  // namespace fx
