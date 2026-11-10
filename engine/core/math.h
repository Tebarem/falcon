#pragma once

// Minimal math for the engine. Matrices are column-major (m[column][row]), matching the
// shaders' column_major layout, so mul(M, v) in Slang equals M * v here.
// Conventions: right-handed, Y up, camera looks down -Z, reverse-Z depth (1 = near).

#include <cmath>

namespace fx {

struct float2 {
    float x, y;
};

struct float3 {
    float x, y, z;
    float3 operator+(float3 b) const { return {x + b.x, y + b.y, z + b.z}; }
    float3 operator-(float3 b) const { return {x - b.x, y - b.y, z - b.z}; }
    float3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float3& operator+=(float3 b) { return *this = *this + b; }
};

struct float4 {
    float x, y, z, w;
};

inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float3 cross(float3 a, float3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(float3 a) { return std::sqrt(dot(a, a)); }
inline float3 normalize(float3 a)
{
    const float l = length(a);
    return l > 0.0f ? a * (1.0f / l) : a;
}

struct float4x4 {
    float m[4][4];  // [column][row]

    static float4x4 identity()
    {
        float4x4 r = {};
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }
};

inline float4x4 operator*(const float4x4& a, const float4x4& b)
{
    float4x4 r = {};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            r.m[c][row] = a.m[0][row] * b.m[c][0] + a.m[1][row] * b.m[c][1] + a.m[2][row] * b.m[c][2] +
                          a.m[3][row] * b.m[c][3];
    return r;
}

// Reverse-Z, infinite far plane: depth = near / -z_view, so precision is best far away.
inline float4x4 perspectiveReverseZ(float fovY, float aspect, float zNear)
{
    const float f = 1.0f / std::tan(fovY * 0.5f);
    float4x4 r = {};
    r.m[0][0] = f / aspect;
    r.m[1][1] = f;
    r.m[2][3] = -1.0f;
    r.m[3][2] = zNear;
    return r;
}

// View matrix from a camera basis (right, up, forward) and position.
inline float4x4 viewFromBasis(float3 right, float3 up, float3 forward, float3 eye)
{
    float4x4 r = float4x4::identity();
    const float3 back = forward * -1.0f;
    r.m[0][0] = right.x; r.m[1][0] = right.y; r.m[2][0] = right.z;
    r.m[0][1] = up.x;    r.m[1][1] = up.y;    r.m[2][1] = up.z;
    r.m[0][2] = back.x;  r.m[1][2] = back.y;  r.m[2][2] = back.z;
    r.m[3][0] = -dot(right, eye);
    r.m[3][1] = -dot(up, eye);
    r.m[3][2] = -dot(back, eye);
    return r;
}

inline constexpr float kPi = 3.14159265358979f;

}  // namespace fx
