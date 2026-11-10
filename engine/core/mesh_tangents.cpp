#include "core/mesh_tangents.h"

#include <mikktspace.h>

#include <cmath>

namespace fx {
namespace {

struct Context {
    const uint32_t* indices;
    size_t indexCount;
    MeshAttribute positions, normals, uvs;
    float* out;
};

const float* attr(const MeshAttribute& a, uint32_t i)
{
    return reinterpret_cast<const float*>(reinterpret_cast<const char*>(a.data) + size_t(i) * a.stride);
}

Context& ctx(const SMikkTSpaceContext* c) { return *static_cast<Context*>(c->m_pUserData); }

int numFaces(const SMikkTSpaceContext* c) { return int(ctx(c).indexCount / 3); }
int numVerticesOfFace(const SMikkTSpaceContext*, int) { return 3; }

void position(const SMikkTSpaceContext* c, float out[], int face, int vert)
{
    const float* p = attr(ctx(c).positions, ctx(c).indices[face * 3 + vert]);
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
}

void normal(const SMikkTSpaceContext* c, float out[], int face, int vert)
{
    const float* n = attr(ctx(c).normals, ctx(c).indices[face * 3 + vert]);
    out[0] = n[0];
    out[1] = n[1];
    out[2] = n[2];
}

void texCoord(const SMikkTSpaceContext* c, float out[], int face, int vert)
{
    const float* t = attr(ctx(c).uvs, ctx(c).indices[face * 3 + vert]);
    out[0] = t[0];
    out[1] = t[1];
}

void setTSpaceBasic(const SMikkTSpaceContext* c, const float tangent[], float sign, int face, int vert)
{
    float* o = ctx(c).out + (size_t(face) * 3 + vert) * 4;
    o[0] = tangent[0];
    o[1] = tangent[1];
    o[2] = tangent[2];
    o[3] = sign < 0.0f ? -1.0f : 1.0f;
}

}  // namespace

bool generateTangents(const uint32_t* indices, size_t indexCount, MeshAttribute positions, MeshAttribute normals,
                      MeshAttribute uvs, float* outTangents)
{
    Context context{indices, indexCount - indexCount % 3, positions, normals, uvs, outTangents};
    SMikkTSpaceInterface iface{};
    iface.m_getNumFaces = numFaces;
    iface.m_getNumVerticesOfFace = numVerticesOfFace;
    iface.m_getPosition = position;
    iface.m_getNormal = normal;
    iface.m_getTexCoord = texCoord;
    iface.m_setTSpaceBasic = setTSpaceBasic;
    SMikkTSpaceContext mikk{&iface, &context};
    // MikkTSpace leaves corners of degenerate triangles untouched: seed every corner with a
    // tangent perpendicular to its normal first.
    for (size_t c = 0; c < context.indexCount; ++c) {
        const float* n = attr(normals, indices[c]);
        const float ax = std::fabs(n[0]) < 0.9f ? 1.0f : 0.0f, ay = 1.0f - ax;
        // t = normalize(a - n * dot(a, n)) with a = x or y.
        const float d = ax * n[0] + ay * n[1];
        float t[3] = {ax - n[0] * d, ay - n[1] * d, -n[2] * d};
        const float l = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        float* o = outTangents + c * 4;
        o[0] = l > 0.0f ? t[0] / l : 1.0f;
        o[1] = l > 0.0f ? t[1] / l : 0.0f;
        o[2] = l > 0.0f ? t[2] / l : 0.0f;
        o[3] = 1.0f;
    }
    return genTangSpaceDefault(&mikk) != 0;
}

}  // namespace fx
