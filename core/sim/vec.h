/*
Derived from Bullet Continuous Collision Detection and Physics Library (LinearMath)
Copyright (c) 2003-2006 Erwin Coumans  http://continuousphysics.com/Bullet/

This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the use of this software.
Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it freely,
subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim that you wrote the
   original software. If you use this software in a product, an acknowledgment in the product
   documentation would be appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be misrepresented as being
   the original software.
3. This notice may not be removed or altered from any source distribution.

ALTERED: reduced to the scalar float operations the Rocket League binary performs, with the
operation order of the shipped x64 code (scalar SSE, no FMA). Every function here is part of the
bit-exact path: do not reorder operands or regroup sums.
*/
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace rlios::sim {

inline uint32_t floatBits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

inline float bitsFloat(uint32_t u) {
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

// sqrtss is IEEE correctly rounded, as is std::sqrt(float) on every target we build for.
inline float sqrtF(float x) {
    return std::sqrt(x);
}

struct Vec3 {
    float x, y, z;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 operator-(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 operator*(const Vec3& a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
// Component-wise product (btVector3 operator*(v, v)).
inline Vec3 mulComp(const Vec3& a, const Vec3& b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}
inline Vec3 neg(const Vec3& a) {
    return {-a.x, -a.y, -a.z};
}
// btVector3::dot: (x*x' + y*y') + z*z'.
inline float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline float length2(const Vec3& a) {
    return dot(a, a);
}
// btVector3::cross.
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline bool isZero(const Vec3& a) {
    return a.x == 0.0f && a.y == 0.0f && a.z == 0.0f;
}

struct Quat {
    float x, y, z, w;
};

// Row-major 3x3 (btMatrix3x3: three row vectors).
struct Mat3 {
    Vec3 r[3];
};

inline Mat3 identityMat3() {
    return {{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
}

// btMatrix3x3 * btVector3.
inline Vec3 mul(const Mat3& m, const Vec3& v) {
    return {dot(m.r[0], v), dot(m.r[1], v), dot(m.r[2], v)};
}

inline Mat3 transpose(const Mat3& m) {
    return {{{m.r[0].x, m.r[1].x, m.r[2].x},
             {m.r[0].y, m.r[1].y, m.r[2].y},
             {m.r[0].z, m.r[1].z, m.r[2].z}}};
}

// btMatrix3x3 * btMatrix3x3: element (i,j) = a[i].x*b[0].j + a[i].y*b[1].j + a[i].z*b[2].j.
inline Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 o;
    for (int i = 0; i < 3; ++i) {
        o.r[i].x = a.r[i].x * b.r[0].x + a.r[i].y * b.r[1].x + a.r[i].z * b.r[2].x;
        o.r[i].y = a.r[i].x * b.r[0].y + a.r[i].y * b.r[1].y + a.r[i].z * b.r[2].y;
        o.r[i].z = a.r[i].x * b.r[0].z + a.r[i].y * b.r[1].z + a.r[i].z * b.r[2].z;
    }
    return o;
}

// btMatrix3x3::getRotation (RVA 0x2779F0, stock).
inline Quat getRotation(const Mat3& m) {
    const float trace = m.r[0].x + m.r[1].y + m.r[2].z;
    float t[4];
    if (trace > 0.0f) {
        float s = sqrtF(trace + 1.0f);
        t[3] = s * 0.5f;
        s = 0.5f / s;
        t[0] = (m.r[2].y - m.r[1].z) * s;
        t[1] = (m.r[0].z - m.r[2].x) * s;
        t[2] = (m.r[1].x - m.r[0].y) * s;
    } else {
        const float e[3][3] = {{m.r[0].x, m.r[0].y, m.r[0].z},
                               {m.r[1].x, m.r[1].y, m.r[1].z},
                               {m.r[2].x, m.r[2].y, m.r[2].z}};
        const int i = e[0][0] < e[1][1] ? (e[1][1] < e[2][2] ? 2 : 1) : (e[0][0] < e[2][2] ? 2 : 0);
        const int j = (i + 1) % 3;
        const int k = (i + 2) % 3;
        float s = sqrtF(e[i][i] - e[j][j] - e[k][k] + 1.0f);
        t[i] = s * 0.5f;
        s = 0.5f / s;
        t[3] = (e[k][j] - e[j][k]) * s;
        t[j] = (e[j][i] + e[i][j]) * s;
        t[k] = (e[k][i] + e[i][k]) * s;
    }
    return {t[0], t[1], t[2], t[3]};
}

// btMatrix3x3::setRotation (RVA 0x27E2D0, stock; 2.0f constant at RVA 0x1A0D8B0).
inline Mat3 setRotation(const Quat& q) {
    const float d = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    const float s = 2.0f / d;
    const float xs = q.x * s, ys = q.y * s, zs = q.z * s;
    const float wx = q.w * xs, wy = q.w * ys, wz = q.w * zs;
    const float xx = q.x * xs, xy = q.x * ys, xz = q.x * zs;
    const float yy = q.y * ys, yz = q.y * zs, zz = q.z * zs;
    return {{{1.0f - (yy + zz), xy - wz, xz + wy},
             {xy + wz, 1.0f - (xx + zz), yz - wx},
             {xz - wy, yz + wx, 1.0f - (xx + yy)}}};
}

struct Transform {
    Mat3 basis;
    Vec3 origin;
};

// btTransform::operator()(v): basis rows dot v, plus origin.
inline Vec3 xform(const Transform& t, const Vec3& v) {
    return {dot(t.basis.r[0], v) + t.origin.x, dot(t.basis.r[1], v) + t.origin.y,
            dot(t.basis.r[2], v) + t.origin.z};
}

// btTransform::invXform(v): transpose(basis) * (v - origin).
inline Vec3 invXform(const Transform& t, const Vec3& v) {
    return mul(transpose(t.basis), v - t.origin);
}

// btTransform::inverse(): (transpose(basis), transpose(basis) * -origin). RVA 0x278F70.
inline Transform inverse(const Transform& t) {
    const Mat3 inv = transpose(t.basis);
    return {inv, mul(inv, neg(t.origin))};
}

} // namespace rlios::sim
