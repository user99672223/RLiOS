/*
Parts of this file are derived from the Bullet Continuous Collision Detection and Physics Library
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

ALTERED: this is not Bullet. It is a reduction of SphereTriangleDetector and
btInternalEdgeUtility to what the Rocket League binary does for the ball, changed wherever the
binary differs. "RVA 0x..." is an address in RocketLeague.exe relative to its image base.
Sums and products are written in the order the shipped x64 code evaluates them; do not regroup.
*/
#include "sim/arena_mesh.h"

#include "sim/crt_math.h"

namespace rlios::sim {

namespace {

const float kSimdEpsilon = bitsFloat(0x34000000u); // FLT_EPSILON, RVA 0x1A0D884
const float kLargeFloat = bitsFloat(0x5D5E0B6Bu);  // BT_LARGE_FLOAT 1e18, RVA 0x1A0D8CC

// btTriangleInfoMap thresholds, measured from the live maps (+0x88, +0x94, +0x98); the dump tool
// refuses to write a file if they ever differ.
const float kConvexEpsilon = 0.0f;
const float kEdgeDistanceThreshold = 0.1f;
const float kMaxEdgeAngleThreshold = bitsFloat(0x40C90FDBu); // 2*pi

// Psyonix addition: when picking the edge nearest to the contact, the segment parameter is
// clamped to [e, 1 - e] instead of [0, 1], so a contact at a vertex is not exactly on two
// edges. e is the double 1e-5 at RVA 0x1A0E3A8 converted to float; its enable byte at
// RVA 0x233488D is set to 1 by the function's own static initializer (measured: 1).
const float kBestEdgeClampLo = static_cast<float>(1.0e-5);
const float kBestEdgeClampHi = 1.0f - kBestEdgeClampLo;

// btTriangleShape::calcNormal (RVA 0x2893C0); the same code is inlined in the detector.
Vec3 triangleNormal(const Vec3 v[3]) {
    const Vec3 e1 = v[1] - v[0];
    const Vec3 e2 = v[2] - v[0];
    const Vec3 n = cross(e1, e2);
    const float inv = 1.0f / sqrtF(length2(n));
    return {inv * n.x, inv * n.y, inv * n.z};
}

// btNearestPointInLineSegment (RVA 0x288940) with Psyonix's clamp range arguments.
Vec3 nearestOnSegment(const Vec3& p, const Vec3& a, const Vec3& b, float lo, float hi) {
    const Vec3 e = b - a;
    const float len2 = length2(e);
    if (!(kSimdEpsilon <= len2))
        return a;
    float t = dot(p - a, e) / len2;
    if (lo <= t) {
        if (!(t <= hi))
            t = hi;
    } else {
        t = lo;
    }
    return {e.x * t + a.x, e.y * t + a.y, e.z * t + a.z};
}

// btClampNormal (RVA 0x288150, stock).
bool clampNormal(const Vec3& edge, const Vec3& triNormal, const Vec3& localContactNormal,
                 float correctedEdgeAngle, Vec3& clamped) {
    Vec3 edgeCross = cross(edge, triNormal);
    const float inv = 1.0f / sqrtF(length2(edgeCross));
    edgeCross = edgeCross * inv;
    const float curAngle =
        crtAtan2f(dot(edgeCross, localContactNormal), dot(triNormal, localContactNormal));
    if ((correctedEdgeAngle < 0.0f && curAngle < correctedEdgeAngle) ||
        (correctedEdgeAngle >= 0.0f && curAngle > correctedEdgeAngle)) {
        const float half = (correctedEdgeAngle - curAngle) * 0.5f;
        const float s = crtSinf(half) / sqrtF(length2(edge));
        const Quat q = {edge.x * s, edge.y * s, edge.z * s, crtCosf(half)};
        clamped = mul(setRotation(q), localContactNormal);
        return true;
    }
    return false;
}

// quatRotate(btQuaternion(axis, angle), v): q * v, then * q.inverse() (RVA 0x281D60).
Vec3 rotateAboutEdge(const Vec3& edge, float angle, const Vec3& v) {
    const float half = angle * 0.5f;
    const float s = crtSinf(half) / sqrtF(length2(edge));
    const float qx = edge.x * s, qy = edge.y * s, qz = edge.z * s;
    const float qw = crtCosf(half);
    const float px = (qw * v.x + qy * v.z) - qz * v.y;
    const float py = (qz * v.x + qw * v.y) - qx * v.z;
    const float pz = (qw * v.z + qx * v.y) - qy * v.x;
    const float pw = (-qx * v.x - qy * v.y) - qz * v.z;
    const float ix = -qx, iy = -qy, iz = -qz;
    return {(px * qw + pw * ix + py * iz) - pz * iy, (py * qw + pw * iy + pz * ix) - px * iz,
            (pz * qw + pw * iz + px * iy) - py * ix};
}

void setContactNormal(ContactPoint& pt, const Transform& meshXf, const Vec3& localNormal) {
    pt.normal = mul(meshXf.basis, localNormal);
    pt.worldB = {pt.worldA.x - pt.distance * pt.normal.x, pt.worldA.y - pt.distance * pt.normal.y,
                 pt.worldA.z - pt.distance * pt.normal.z};
    pt.localB = invXform(meshXf, pt.worldB);
}

} // namespace

// SphereTriangleDetector::collide (RVA 0x28A8F0). Psyonix change: the triangle is ONE-SIDED.
// Stock Bullet flips the normal when the centre is behind the plane; here a centre behind the
// triangle is simply no contact.
bool sphereTriangle(const Vec3 v[3], const Vec3& c, float radius, float breakingThreshold,
                    Vec3& point, Vec3& normal, float& depth) {
    const float radiusWithThreshold = radius + breakingThreshold;
    const Vec3 n = triangleNormal(v);
    const float dist = dot(c - v[0], n);
    if (dist < 0.0f)
        return false;
    if (radiusWithThreshold <= dist)
        return false;

    // pointInTriangle: same side of all three edge planes.
    const float r1 = dot(cross(v[1] - v[0], n), c - v[0]);
    const float r2 = dot(cross(v[2] - v[1], n), c - v[1]);
    const float r3 = dot(cross(v[0] - v[2], n), c - v[2]);
    Vec3 contact;
    if ((0.0f < r1 && 0.0f < r2 && 0.0f < r3) || (r1 <= 0.0f && r2 <= 0.0f && r3 <= 0.0f)) {
        contact = {c.x - n.x * dist, c.y - n.y * dist, c.z - n.z * dist};
    } else {
        // Nearest point on the three edges (SegmentSqrDistance), within the contact radius.
        float minDistSqr = radiusWithThreshold * radiusWithThreshold;
        bool has = false;
        for (int i = 0; i < 3; ++i) {
            const Vec3& from = v[i];
            const Vec3& to = v[(i + 1) % 3];
            const Vec3 e = to - from;
            Vec3 diff = c - from;
            float t = dot(diff, e);
            if (t <= 0.0f) {
                t = 0.0f;
            } else {
                const float dotVV = length2(e);
                if (dotVV <= t) {
                    diff = diff - e;
                    t = 1.0f;
                } else {
                    t = t / dotVV;
                    diff = {diff.x - t * e.x, diff.y - t * e.y, diff.z - t * e.z};
                }
            }
            const Vec3 nearest = {e.x * t + from.x, e.y * t + from.y, e.z * t + from.z};
            const float distSqr = length2(diff);
            if (distSqr < minDistSqr) {
                has = true;
                minDistSqr = distSqr;
                contact = nearest;
            }
        }
        if (!has)
            return false;
    }

    const Vec3 d = c - contact;
    const float distSqr = length2(d);
    if (radiusWithThreshold * radiusWithThreshold <= distSqr)
        return false;
    if (distSqr <= kSimdEpsilon) {
        normal = n;
        depth = -radius;
    } else {
        const float inv = 1.0f / sqrtF(length2(d));
        normal = {inv * d.x, d.y * inv, d.z * inv};
        depth = -(radius - sqrtF(distSqr));
    }
    point = contact;
    return true;
}

// btAdjustInternalEdgeContacts (RVA 0x2865D0) as called by the game's contact-added callback
// (RVA 0x415850) with normalAdjustFlags 0: front-facing only, no double-sided handling.
// Stock Bullet 2.83 logic (nearest "best" edge decides) with Psyonix changes:
//  - the best-edge search uses the clamp range above;
//  - an edge is processed if it is the best edge OR the contact is (within FLT_EPSILON) on it.
void adjustInternalEdge(ContactPoint& pt, const Transform& meshXf, const Vec3 v[3],
                        const MeshTriInfo& info) {
    if (info.flags == -1)
        return;

    const Vec3 triNormal = triangleNormal(v);
    const Vec3 contact = pt.localB;

    Vec3 localNormal = mul(transpose(meshXf.basis), pt.normal);
    const float invLen = 1.0f / sqrtF(length2(localNormal));
    localNormal = localNormal * invLen;

    int bestEdge = -1;
    float distToBestEdge = kLargeFloat;
    bool onEdge[3] = {false, false, false};
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(info.edgeAngle[k]) < kMaxEdgeAngleThreshold) {
            const Vec3 nearest =
                nearestOnSegment(contact, v[k], v[(k + 1) % 3], kBestEdgeClampLo, kBestEdgeClampHi);
            const float len = sqrtF(length2(contact - nearest));
            if (len < distToBestEdge) {
                bestEdge = k;
                distToBestEdge = len;
            }
            if (len < kSimdEpsilon)
                onEdge[k] = true;
        }
    }

    bool isNearEdge = false;
    int numConcaveEdgeHits = 0;
    for (int k = 0; k < 3; ++k) {
        const Vec3& a = v[k];
        const Vec3& b = v[(k + 1) % 3];
        const float angle = info.edgeAngle[k];
        if (!(std::fabs(angle) < kMaxEdgeAngleThreshold))
            continue;
        const Vec3 nearest = nearestOnSegment(contact, a, b, 0.0f, 1.0f);
        const float len = sqrtF(length2(contact - nearest));
        if (!(len < kEdgeDistanceThreshold && (bestEdge == k || onEdge[k])))
            continue;
        const Vec3 edge = a - b;
        isNearEdge = true;
        if (angle == 0.0f) {
            ++numConcaveEdgeHits;
            continue;
        }
        const bool isEdgeConvex = (info.flags & (1 << k)) != 0;
        const float swapFactor = isEdgeConvex ? 1.0f : -1.0f;
        const Vec3 nA = triNormal * swapFactor;
        Vec3 computedNormalB = rotateAboutEdge(edge, angle, triNormal);
        if ((info.flags & (8 << k)) != 0) // TRI_INFO_V*_SWAP_NORMALB
            computedNormalB = computedNormalB * -1.0f;
        const Vec3 nB = computedNormalB * swapFactor;
        const float nDotA = dot(localNormal, nA);
        const float nDotB = dot(localNormal, nB);
        const bool backFacingNormal = !(kConvexEpsilon <= nDotA) && !(kConvexEpsilon <= nDotB);
        if (backFacingNormal) {
            ++numConcaveEdgeHits;
            continue;
        }
        // The first edge clamps the normalized normal computed above; the other two recompute
        // it from the (possibly already adjusted) world normal without normalizing.
        const Vec3 n = k == 0 ? localNormal : mul(transpose(meshXf.basis), pt.normal);
        Vec3 clamped;
        if (clampNormal(edge, nA, n, angle, clamped) && 0.0f < dot(triNormal, clamped))
            setContactNormal(pt, meshXf, clamped);
    }

    if (!isNearEdge || numConcaveEdgeHits == 0)
        return;
    if (dot(triNormal, localNormal) < 0.0f)
        return;
    setContactNormal(pt, meshXf, triNormal);
}

} // namespace rlios::sim
