#include "level/splines.h"

#include "core/json.h"

#include <algorithm>
#include <cmath>

namespace fx::level {

namespace {

uint32_t segmentsOf(const Spline& s)
{
    const uint32_t n = uint32_t(s.points.size());
    return n < 2 ? 0u : s.closed ? n : n - 1;
}

// Point i's tangent (Catmull-Rom; a corner's none: straight in and out of it).
float3 tangentAt(const Spline& s, uint32_t i)
{
    const uint32_t n = uint32_t(s.points.size());
    if (s.points[i].corner || n < 2)
        return {0, 0, 0};
    if (!s.closed && i == 0)
        return s.points[1].position - s.points[0].position;
    if (!s.closed && i == n - 1)
        return s.points[n - 1].position - s.points[n - 2].position;
    return (s.points[(i + 1) % n].position - s.points[(i + n - 1) % n].position) * 0.5f;
}

float3 evaluate(const Spline& s, uint32_t segment, float t)
{
    const uint32_t n = uint32_t(s.points.size()), a = segment, b = (segment + 1) % n;
    const float3 p0 = s.points[a].position, p1 = s.points[b].position, m0 = tangentAt(s, a), m1 = tangentAt(s, b);
    const float t2 = t * t, t3 = t2 * t;
    return p0 * (2 * t3 - 3 * t2 + 1) + m0 * (t3 - 2 * t2 + t) + p1 * (-2 * t3 + 3 * t2) + m1 * (t3 - t2);
}

// The curve finely (32 a segment) with each point's distance along it; where each control point is.
struct Dense {
    std::vector<float3> points;
    std::vector<float> distance;
    std::vector<float> at;  // each control point's distance
};

Dense dense(const Spline& s)
{
    Dense d;
    const uint32_t segments = segmentsOf(s);
    if (segments == 0) {
        if (!s.points.empty()) {
            d.points.push_back(s.points[0].position);
            d.distance.push_back(0.0f);
            d.at.push_back(0.0f);
        }
        return d;
    }
    float total = 0.0f;
    for (uint32_t i = 0; i < segments; ++i)
        for (uint32_t k = 0; k < 32; ++k) {
            const float3 p = evaluate(s, i, float(k) / 32.0f);
            if (!d.points.empty())
                total += length(p - d.points.back());
            if (k == 0)
                d.at.push_back(total);
            d.points.push_back(p);
            d.distance.push_back(total);
        }
    const float3 last = s.closed ? s.points[0].position : s.points.back().position;
    total += length(last - d.points.back());
    d.points.push_back(last);
    d.distance.push_back(total);
    if (!s.closed)
        d.at.push_back(total);
    return d;
}

float3 positionAt(const Dense& d, float distance)
{
    if (d.points.size() < 2)
        return d.points.empty() ? float3{0, 0, 0} : d.points[0];
    const float length = d.distance.back();
    distance = std::clamp(distance, 0.0f, length);
    const size_t i = size_t(std::upper_bound(d.distance.begin(), d.distance.end(), distance) - d.distance.begin());
    const size_t b = std::min(std::max(i, size_t(1)), d.points.size() - 1), a = b - 1;
    const float span = d.distance[b] - d.distance[a], t = span > 1e-9f ? (distance - d.distance[a]) / span : 0.0f;
    return d.points[a] + (d.points[b] - d.points[a]) * t;
}

std::vector<float2> profileOf(const LoftSettings& l)
{
    const float w = l.width * 0.5f, h = l.height;
    if (l.profile == "road")
        return {{-w, -h}, {w, -h}, {w, 0}, {-w, 0}};
    if (l.profile == "kerb" || l.profile == "wall")
        return {{-w, 0}, {w, 0}, {w, h}, {-w, h}};
    if (l.profile == "rail")
        return {{-w, -h * 0.5f}, {w, -h * 0.5f}, {w, h * 0.5f}, {-w, h * 0.5f}};
    if (l.profile == "pipe") {
        std::vector<float2> ring;
        const uint32_t sides = std::clamp(l.sides, 3u, 128u);
        for (uint32_t k = 0; k < sides; ++k) {
            const float a = float(k) / float(sides) * 6.2831853f;
            ring.push_back({w * std::cos(a), w * std::sin(a)});
        }
        return ring;
    }
    if (l.profile == "custom")
        return l.points;
    return {};
}

}  // namespace

bool Spline::operator==(const Spline& o) const
{
    if (closed != o.closed || points.size() != o.points.size())
        return false;
    for (size_t i = 0; i < points.size(); ++i)
        if (points[i].corner != o.points[i].corner || points[i].position.x != o.points[i].position.x ||
            points[i].position.y != o.points[i].position.y || points[i].position.z != o.points[i].position.z)
            return false;
    return true;
}

bool LoftSettings::operator==(const LoftSettings& o) const
{
    if (profile != o.profile || width != o.width || height != o.height || sides != o.sides || step != o.step || uvLength != o.uvLength ||
        material != o.material || points.size() != o.points.size())
        return false;
    for (size_t i = 0; i < points.size(); ++i)
        if (points[i].x != o.points[i].x || points[i].y != o.points[i].y)
            return false;
    return true;
}

std::vector<float3> splineCurve(const Spline& s, float step)
{
    std::vector<float3> out;
    const uint32_t segments = segmentsOf(s);
    if (segments == 0) {
        for (const SplinePoint& p : s.points)
            out.push_back(p.position);
        return out;
    }
    step = std::max(step, 0.01f);
    for (uint32_t i = 0; i < segments; ++i) {
        const float chord = length(s.points[(i + 1) % s.points.size()].position - s.points[i].position);
        const uint32_t n = std::clamp(uint32_t(std::ceil(chord / step)), 1u, 256u);
        for (uint32_t k = 0; k < n; ++k)
            out.push_back(evaluate(s, i, float(k) / float(n)));
    }
    out.push_back(s.closed ? s.points[0].position : s.points.back().position);
    return out;
}

float splineLength(const Spline& s)
{
    const Dense d = dense(s);
    return d.distance.empty() ? 0.0f : d.distance.back();
}

std::vector<SplineFrame> splineFrames(const Spline& s, float step)
{
    std::vector<SplineFrame> frames;
    const Dense d = dense(s);
    if (d.points.size() < 2)
        return frames;
    const float total = d.distance.back();
    step = std::max(step, 0.01f);
    // Where: every step, its control points (a corner gets a frame right on it), its end.
    std::vector<float> where;
    for (float x = 0.0f; x < total; x += step)
        where.push_back(x);
    where.insert(where.end(), d.at.begin(), d.at.end());
    where.push_back(total);
    std::sort(where.begin(), where.end());
    std::vector<float> kept;
    for (const float x : where)
        if (kept.empty() || x - kept.back() > step * 0.2f)
            kept.push_back(x);
        else if (x == total || std::find(d.at.begin(), d.at.end(), x) != d.at.end())
            kept.back() = x;  // (a control point or the end wins over a step next to it)
    float3 lastSide = {0, 0, 1};
    const float e = std::min(step * 0.25f, 0.05f);
    for (const float x : kept) {
        SplineFrame f;
        f.distance = x;
        f.position = positionAt(d, x);
        // Which way it runs: through a little either side (the bisector at a corner); a closed
        // one wraps round.
        float3 a, b;
        if (s.closed) {
            const auto wrap = [&](float y) { return positionAt(d, std::fmod(std::fmod(y, total) + total, total)); };
            a = wrap(x - e), b = wrap(x + e);
        } else {
            a = positionAt(d, x - e), b = positionAt(d, x + e);
        }
        f.tangent = length(b - a) > 1e-7f ? normalize(b - a) : float3{1, 0, 0};
        float3 side = cross(f.tangent, float3{0, 1, 0});
        side = length(side) > 1e-4f ? normalize(side) : lastSide;  // (straight up or down: as before)
        f.side = lastSide = side;
        f.up = normalize(cross(side, f.tangent));
        frames.push_back(f);
    }
    return frames;
}

SplineFrame frameAt(const std::vector<SplineFrame>& frames, float distance)
{
    if (frames.empty())
        return {};
    if (distance <= frames.front().distance || frames.size() == 1) {
        SplineFrame f = frames.front();
        f.position = f.position + f.tangent * (distance - f.distance);
        f.distance = distance;
        return f;
    }
    if (distance >= frames.back().distance) {
        SplineFrame f = frames.back();
        f.position = f.position + f.tangent * (distance - f.distance);
        f.distance = distance;
        return f;
    }
    const auto it = std::upper_bound(frames.begin(), frames.end(), distance, [](float d, const SplineFrame& f) { return d < f.distance; });
    const SplineFrame& b = *it;
    const SplineFrame& a = *(it - 1);
    const float t = (distance - a.distance) / std::max(b.distance - a.distance, 1e-9f);
    SplineFrame f;
    f.distance = distance;
    f.position = a.position + (b.position - a.position) * t;
    f.tangent = normalize(a.tangent + (b.tangent - a.tangent) * t);
    const float3 up = a.up + (b.up - a.up) * t;
    f.side = normalize(cross(f.tangent, up));
    f.up = normalize(cross(f.side, f.tangent));
    return f;
}

LoftSettings loftPreset(const std::string& profile)
{
    LoftSettings l;
    l.profile = profile;
    if (profile == "road")
        l.width = 4.0f, l.height = 0.2f;
    else if (profile == "kerb")
        l.width = 0.3f, l.height = 0.15f;
    else if (profile == "wall")
        l.width = 0.2f, l.height = 2.0f;
    else if (profile == "rail")
        l.width = 0.08f, l.height = 0.08f, l.step = 0.25f;
    else if (profile == "pipe")
        l.width = 0.3f, l.height = 0.3f, l.step = 0.25f;
    return l;
}

PolyMesh loftSpline(const Spline& s, const LoftSettings& loft)
{
    PolyMesh m;
    m.materials.push_back(loft.material);
    const std::vector<float2> profile = profileOf(loft);
    const std::vector<SplineFrame> frames = splineFrames(s, loft.step);
    if (profile.size() < 3 || frames.size() < 2)
        return m;
    const bool pipe = loft.profile == "pipe";
    const float uv = loft.uvLength > 0.0f ? loft.uvLength : std::max(loft.width, 0.01f);
    // Rings (a closed spline's last frame is its first again: not repeated).
    const size_t rings = s.closed ? frames.size() - 1 : frames.size();
    const uint32_t p = uint32_t(profile.size());
    for (size_t r = 0; r < rings; ++r)
        for (const float2& q : profile)
            m.positions.push_back(frames[r].position + frames[r].side * q.x + frames[r].up * q.y);
    // How far round the profile each of its corners is (v), and the faces between the rings.
    std::vector<float> around(p + 1, 0.0f);
    for (uint32_t k = 0; k < p; ++k)
        around[k + 1] = around[k] + std::hypot(profile[(k + 1) % p].x - profile[k].x, profile[(k + 1) % p].y - profile[k].y);
    const size_t spans = s.closed ? rings : rings - 1;
    for (size_t r = 0; r < spans; ++r) {
        const size_t n = (r + 1) % rings;
        const float u0 = frames[r].distance / uv, u1 = frames[r + 1].distance / uv;
        for (uint32_t k = 0; k < p; ++k) {
            const uint32_t k1 = (k + 1) % p;
            const uint32_t a = uint32_t(r * p + k), b = uint32_t(n * p + k), c = uint32_t(n * p + k1), d = uint32_t(r * p + k1);
            const uint32_t f = m.addFace({a, b, c, d}, 0, pipe ? 1u : k + 1);
            m.faces[f].uv.mode = UvMode::Manual;
            m.cornerUvs.resize(m.corners.size());
            float2* t = &m.cornerUvs[m.faces[f].first];
            t[0] = {u0, around[k] / uv}, t[1] = {u1, around[k] / uv}, t[2] = {u1, around[k + 1] / uv}, t[3] = {u0, around[k + 1] / uv};
        }
    }
    // Caps on an open one's ends.
    if (!s.closed) {
        for (int end = 0; end < 2; ++end) {
            std::vector<uint32_t> loop;
            const uint32_t base = end == 0 ? 0u : uint32_t((rings - 1) * p);
            for (uint32_t k = 0; k < p; ++k)
                loop.push_back(base + (end == 0 ? k : p - 1 - k));
            const uint32_t f = m.addFace(loop);
            m.faces[f].uv.mode = UvMode::Manual;
            m.cornerUvs.resize(m.corners.size());
            for (uint32_t k = 0; k < p; ++k) {
                const float2 q = profile[end == 0 ? k : p - 1 - k];
                m.cornerUvs[m.faces[f].first + k] = {q.x / uv, q.y / uv};
            }
        }
    }
    return m;
}

void readSpline(const Json& j, Spline& s, LoftSettings& loft)
{
    s = {};
    if (const Json* points = j.get("points"))
        for (const Json& p : points->array)
            if (p.array.size() >= 3)
                s.points.push_back({{float(p.array[0].number), float(p.array[1].number), float(p.array[2].number)},
                                    p.array.size() > 3 && p.array[3].number != 0.0});
    s.closed = j.getBool("closed");
    loft = {};
    if (const Json* l = j.get("loft"); l && l->isObject()) {
        loft = loftPreset(l->getString("profile"));
        loft.width = float(l->getNumber("width", loft.width));
        loft.height = float(l->getNumber("height", loft.height));
        loft.sides = uint32_t(std::clamp(l->getNumber("sides", loft.sides), 3.0, 128.0));
        loft.step = std::max(float(l->getNumber("step", loft.step)), 0.02f);
        loft.uvLength = float(l->getNumber("uvLength", loft.uvLength));
        loft.material = l->getString("material");
        if (const Json* points = l->get("points"))
            for (const Json& p : points->array)
                if (p.array.size() >= 2)
                    loft.points.push_back({float(p.array[0].number), float(p.array[1].number)});
    }
}

void writeSpline(JsonWriter& w, const char* key, const Spline& s, const LoftSettings& loft)
{
    w.beginObject(key).beginArray("points");
    for (const SplinePoint& p : s.points) {
        w.beginArray().value(nullptr, double(p.position.x)).value(nullptr, double(p.position.y)).value(nullptr, double(p.position.z));
        if (p.corner)
            w.value(nullptr, 1u);
        w.endArray();
    }
    w.endArray().value("closed", s.closed);
    if (!loft.profile.empty()) {
        w.beginObject("loft").value("profile", loft.profile).value("width", double(loft.width)).value("height", double(loft.height))
            .value("sides", loft.sides).value("step", double(loft.step)).value("uvLength", double(loft.uvLength)).value("material", loft.material);
        if (!loft.points.empty()) {
            w.beginArray("points");
            for (const float2& p : loft.points)
                w.beginArray().value(nullptr, double(p.x)).value(nullptr, double(p.y)).endArray();
            w.endArray();
        }
        w.endObject();
    }
    w.endObject();
}

}  // namespace fx::level
