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

ALTERED: this is not Bullet. It is a reduction of Bullet 2.8x (btDiscreteDynamicsWorld,
btSequentialImpulseConstraintSolver, btPersistentManifold, btConvexPlaneCollisionAlgorithm,
btTransformUtil) to one sphere and one plane, changed wherever the Rocket League binary differs.

Evidence notation: "RVA 0x..." is an address in RocketLeague.exe relative to image base
0x140000000 (build measured 2026-11). "measured" means read from the running game's memory.
Sums and products are written in the order the shipped x64 code evaluates them; do not regroup.
*/
#include "sim/ball_world.h"

#include "sim/crt_math.h"

namespace rlios::sim {

namespace {

// ---- constants -------------------------------------------------------------------------------

// Tick length. Measured: the tick function (RVA 0xEEB3F0) passes these bits every tick.
const float kDt = bitsFloat(0x3C088889u); // 1/120

// Ball body, measured from the live btRigidBody (offsets are stock Bullet 2.82 btRigidBody).
// btSphereShape radius (shape +0x30 and margin +0x40), measured. It is 91.25 uu * 0.02f, which is
// two ulps below 1.825f; the nearest float to 1.825 diverges at the first floor contact.
const float kBallRadius = bitsFloat(0x3FE99998u);
const float kBallInvMass = bitsFloat(0x3D088888u);     // +0x180
const float kGravityForceZ = bitsFloat(0xC3C30001u);   // +0x1A8: -13 BU/s^2 * mass
const float kInvInertiaLocal = bitsFloat(0x3CCCF771u); // +0x1C0 (all three axes)
// applyDamping (RVA 0x2C21D0) multiplies the linear velocity by powf(1 - linDamping, dt) with
// linDamping = 0.03f (+0x1F0). The product below is that powf result as observed in free fall
// (every free-fall tick of the reference recording reproduces with it). Angular damping is 0,
// and powf(1, dt) == 1, so the angular velocity is multiplied by exactly 1.
const float kLinDampFactor = bitsFloat(0x3F7FEF5Eu);
const float kBallFriction = 0.35f;     // measured manifold point m_combinedFriction, ball vs floor
const float kBallRestitution = 0.6f;   // measured manifold point m_combinedRestitution
const float kBodyMaxLinSpeed = 120.0f; // +0x368 (Psyonix field), BU/s
const float kBodyMaxAngSpeed = 6.0f;   // +0x36C (Psyonix field), rad/s
const float kLinSleepThreshold = 0.8f; // +0x20C
const float kAngSleepThreshold = 1.0f; // +0x210
const float kDeactivationTime = 2.0f;  // gDeactivationTime, RVA 0x2232038

// Ball actor limits used when building the RBState (actor+0x570 / +0x574).
const float kMaxLinSpeedUU = 6000.0f;
const float kMaxAngSpeed = 6.0f;

const float kUUToBU = 0.02f; // RVA 0x1A0F3EC
const float kBUToUU = 50.0f; // RVA 0x1B34120

// Floor: Psyonix bounded plane shape (shape type 28, vtable RVA 0x1A0E8B8), local normal +Z,
// plane constant 0, identity world transform (measured), collision margin 0.
struct Plane {
    Transform xf;
    float halfX; // shape +0x20
    float halfY; // shape +0x24
};
Vec3 v3b(uint32_t x, uint32_t y, uint32_t z) {
    return {bitsFloat(x), bitsFloat(y), bitsFloat(z)};
}
// The six bounded planes of Stadium_P, in Bullet world order (collision objects 0..5), read
// from the live world (btCollisionObject +0x10 transform; shape +0x20/+0x24 half extents). The
// ceiling and one side wall carry the rounding noise of the game's rotator conversion, so the
// bases are given as bit patterns. Back walls, corners, ramps and goals are triangle meshes.
const Plane kPlanes[kNumPlanes] = {
    // 0, 1: goal floors behind the goal lines
    {{{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}},
      v3b(0x00000000u, 0xC2DF5C29u, 0x00000000u)},
     bitsFloat(0x418D8EC9u),
     bitsFloat(0x410D8EC9u)},
    {{{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}},
      v3b(0x00000000u, 0x42DF5C29u, 0x00000000u)},
     bitsFloat(0x418D8EC9u),
     bitsFloat(0x410D8EC9u)},
    // 2: main floor
    {{{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}}, {0.0f, 0.0f, 0.0f}},
     bitsFloat(0x42AE5365u),
     bitsFloat(0x42F9096Bu)},
    // 3: ceiling
    {{{{v3b(0xBF800000u, 0xB3BBBD2Eu, 0xA809ADECu), v3b(0xB3BBBD2Eu, 0x3F800000u, 0x33BBBD2Eu),
        v3b(0x9C000000u, 0x33BBBD2Eu, 0xBF800000u)}},
      v3b(0x00000000u, 0x00000000u, 0x4223D70Au)},
     bitsFloat(0x42AE5365u),
     bitsFloat(0x42F9096Bu)},
    // 4: side wall at -X
    {{{{{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}}},
      v3b(0xC2A3D70Au, 0x00000000u, 0x419EB852u)},
     bitsFloat(0x41C73ABCu),
     bitsFloat(0x42F9096Bu)},
    // 5: side wall at +X
    {{{{v3b(0x34400000u, 0x9CC00000u, 0xBF7FFFFFu), v3b(0x2809ADECu, 0x3F800000u, 0x00000000u),
        v3b(0x3F7FFFFFu, 0xA809ADEBu, 0x34400000u)}},
      v3b(0x42A3D70Au, 0x00000000u, 0x419EB852u)},
     bitsFloat(0x41C73ABCu),
     bitsFloat(0x42F9096Bu)},
};
// Ball-floor manifold m_contactBreakingThreshold (manifold +0x464), measured.
const float kBreakingThreshold = bitsFloat(0x3D1C0EBDu);
const float kSphereAabbMargin = bitsFloat(0x3D23D70Au); // 0.04f, RVA 0x1A0E388

// btContactSolverInfo, measured from the world (world+0x108 solver, info block in the world).
const int kNumIterations = 10;
const float kErp2 = 0.8f; // split impulse is on, threshold 1e30: erp2 always applies
const float kSplitImpulseTurnErp = 0.1f;
const float kLinearSlop = 0.0f;
const float kContactUpperLimit = 1.0e10f;

const float kSimdEpsilon = bitsFloat(0x34000000u);            // FLT_EPSILON, RVA 0x1A0D884
const float kAngularMotionThreshold = bitsFloat(0x3F490FDBu); // pi/4, RVA 0x1A0EAA0

// ---- small helpers ---------------------------------------------------------------------------

bool isSleeping(const BallBody& b) {
    return b.activation == kIslandSleeping;
}

// btTransformUtil::integrateTransform (RVA 0x2A94B0, stock).
Transform integrateTransform(const Transform& cur, const Vec3& linVel, const Vec3& angVel,
                             float dt) {
    Transform out;
    out.origin = {dt * linVel.x + cur.origin.x, dt * linVel.y + cur.origin.y,
                  dt * linVel.z + cur.origin.z};
    float angle = sqrtF(length2(angVel));
    if (kAngularMotionThreshold < angle * dt)
        angle = kAngularMotionThreshold / dt;
    float k;
    if (angle < 0.001f) {
        // 0.020833333f is RVA 0x1A0EA80 (1/48).
        k = dt * 0.5f - dt * dt * dt * 0.020833333333f * angle * angle;
    } else {
        k = crtSinf(angle * 0.5f * dt) / angle;
    }
    const Vec3 ax = {angVel.x * k, angVel.y * k, angVel.z * k};
    const float c = crtCosf(angle * dt * 0.5f);
    const Quat q0 = getRotation(cur.basis);
    // btQuaternion dorn(ax, c) * q0
    Quat p;
    p.x = (ax.x * q0.w + c * q0.x + q0.z * ax.y) - ax.z * q0.y;
    p.y = (c * q0.y + q0.w * ax.y + ax.z * q0.x) - ax.x * q0.z;
    p.z = (c * q0.z + ax.z * q0.w + ax.x * q0.y) - ax.y * q0.x;
    p.w = ((c * q0.w - ax.x * q0.x) - ax.y * q0.y) - ax.z * q0.z;
    const float len = sqrtF(p.y * p.y + p.x * p.x + p.z * p.z + p.w * p.w);
    const float inv = 1.0f / len;
    p = {p.x * inv, p.y * inv, p.z * inv, p.w * inv};
    out.basis = setRotation(p);
    return out;
}

// Psyonix speed clamp shared by applyDamping (RVA 0x2C21D0): v * (1/|v|) * max.
void clampSpeed(Vec3& v, float maxSpeed) {
    if (0.0f < maxSpeed && maxSpeed * maxSpeed < length2(v)) {
        const float inv = 1.0f / sqrtF(length2(v));
        v = {v.x * inv * maxSpeed, v.y * inv * maxSpeed, v.z * inv * maxSpeed};
    }
}

// RBState velocity clamp in RVA 0xEAAE40.
// NOTE: not exercised by any reference recording yet (needs > 6000 uu/s or > 6 rad/s).
void clampRBVel(Vec3& v, float maxSpeed) {
    const float l2 = length2(v);
    if (l2 > maxSpeed * maxSpeed) {
        Vec3 n = {0.0f, 0.0f, 0.0f};
        if (l2 == 1.0f) {
            n = v;
        } else if (l2 >= 1.0e-8f) {
            const float inv = 1.0f / sqrtF(l2);
            n = v * inv;
        }
        v = n * maxSpeed;
    }
}

// ---- pre-step: quantize the body through the RBState --------------------------------------

void preStep(BallBody& b) {
    RBState s = ballRBState(b);
    quantize(s);
    // RVA 0xEF0300: write the (quantized) RBState back into the Bullet body.
    b.xf.origin = {s.pos.x * kUUToBU, s.pos.y * kUUToBU, s.pos.z * kUUToBU};
    b.xf.basis = setRotation(s.rot);
    b.linVel = {s.linVel.x * kUUToBU, s.linVel.y * kUUToBU, s.linVel.z * kUUToBU};
    b.angVel = s.angVel;
}

// ---- collision: sphere vs bounded plane, persistent manifold ---------------------------------

// btPersistentManifold::sortCachedPoints (stock 2.8x). Chooses which of four cached points a
// fifth one replaces. NOTE: stock Bullet, not yet confirmed against the binary or a recording.
int sortCachedPoints(const Manifold& m, const ContactPoint& pt) {
    int maxPenetrationIndex = -1;
    float maxPenetration = pt.distance;
    for (int i = 0; i < 4; ++i) {
        if (m.pts[i].distance < maxPenetration) {
            maxPenetrationIndex = i;
            maxPenetration = m.pts[i].distance;
        }
    }
    float res[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (maxPenetrationIndex != 0)
        res[0] = length2(cross(pt.localA - m.pts[1].localA, m.pts[3].localA - m.pts[2].localA));
    if (maxPenetrationIndex != 1)
        res[1] = length2(cross(pt.localA - m.pts[0].localA, m.pts[3].localA - m.pts[2].localA));
    if (maxPenetrationIndex != 2)
        res[2] = length2(cross(pt.localA - m.pts[0].localA, m.pts[3].localA - m.pts[1].localA));
    if (maxPenetrationIndex != 3)
        res[3] = length2(cross(pt.localA - m.pts[0].localA, m.pts[2].localA - m.pts[1].localA));
    int best = -1;
    float bestVal = -1.0e18f;
    for (int i = 0; i < 4; ++i) {
        const float v = std::fabs(res[i]);
        if (v > bestVal) {
            best = i;
            bestVal = v;
        }
    }
    return best;
}

// btManifoldResult::addContactPoint (stock): cache lookup by local point on A. Returns the index
// of the manifold point that now holds the contact.
int addContactPoint(Manifold& m, const Transform& ballXf, const Transform& planeXf,
                    const Vec3& normal, const Vec3& pointOnFloor, float depth) {
    ContactPoint pt;
    const Vec3 pointA = pointOnFloor + normal * depth;
    pt.localA = invXform(ballXf, pointA);
    pt.localB = invXform(planeXf, pointOnFloor);
    pt.worldA = pointA;
    pt.worldB = pointOnFloor;
    pt.normal = normal;
    pt.distance = depth;

    // btPersistentManifold::getCacheEntry
    float shortest = kBreakingThreshold * kBreakingThreshold;
    int nearest = -1;
    for (int i = 0; i < m.count; ++i) {
        const Vec3 d = m.pts[i].localA - pt.localA;
        const float dist = dot(d, d);
        if (dist < shortest) {
            shortest = dist;
            nearest = i;
        }
    }
    if (nearest < 0)
        nearest = m.count == 4 ? sortCachedPoints(m, pt) : m.count++;
    m.pts[nearest] = pt;
    return nearest;
}

// btPersistentManifold::refreshContactPoints (RVA 0x2AC480, stock).
void refreshContactPoints(Manifold& m, const Transform& ballXf, const Transform& planeXf) {
    for (int i = m.count - 1; i >= 0; --i) {
        ContactPoint& p = m.pts[i];
        p.worldA = xform(ballXf, p.localA);
        p.worldB = xform(planeXf, p.localB);
        p.distance = dot(p.worldA - p.worldB, p.normal);
    }
    for (int i = m.count - 1; i >= 0; --i) {
        const ContactPoint& p = m.pts[i];
        bool remove;
        if (p.distance <= kBreakingThreshold) {
            const Vec3 projected = p.worldA - p.normal * p.distance;
            const Vec3 diff = p.worldB - projected;
            remove = kBreakingThreshold * kBreakingThreshold < dot(diff, diff);
        } else {
            remove = true;
        }
        if (remove) {
            const int last = m.count - 1;
            if (i != last)
                m.pts[i] = m.pts[last];
            m.count = last;
        }
    }
}

// Convex vs Psyonix bounded plane, processCollision (RVA 0x2942B0). The sphere takes the
// single-contact path (the perturbation loop only runs for polyhedral shapes, type < 7).
void collidePlane(BallWorld& w, int index) {
    const Transform& cv = w.ball.xf;
    const Transform& pl = kPlanes[index].xf;
    Manifold& manifold = w.manifolds[index];

    // Direction of -planeNormal in ball space. The plane normal is the literal (0, 0, 1), which
    // the compiler folded into "(m0 * 0 + m1 * 0) - m2 * 1".
    const Mat3 m = mul(transpose(cv.basis), pl.basis);
    const Vec3 dir = {(m.r[0].x * 0.0f + m.r[0].y * 0.0f) - m.r[0].z * 1.0f,
                      (m.r[1].x * 0.0f + m.r[1].y * 0.0f) - m.r[1].z * 1.0f,
                      (m.r[2].x * 0.0f + m.r[2].y * 0.0f) - m.r[2].z * 1.0f};

    // btSphereShape::localGetSupportingVertex: zero + radius * normalized(dir).
    Vec3 vn = dir;
    if (length2(vn) < kSimdEpsilon * kSimdEpsilon)
        vn = {-1.0f, -1.0f, -1.0f};
    const float invLen = 1.0f / sqrtF(length2(vn));
    vn = vn * invLen;
    const Vec3 vtx = {0.0f + kBallRadius * vn.x, 0.0f + kBallRadius * vn.y,
                      0.0f + kBallRadius * vn.z};

    // Support vertex in plane space.
    const Transform pinv = inverse(pl);
    const Mat3 cb = mul(pinv.basis, cv.basis);
    const Vec3 co = xform(pinv, cv.origin);
    const Vec3 vp = {dot(cb.r[0], vtx) + co.x, dot(cb.r[1], vtx) + co.y, dot(cb.r[2], vtx) + co.z};
    const float dist = vp.z;

    // Psyonix: the plane is bounded. No contact if the gap reaches the breaking threshold or the
    // point is outside the (threshold-inflated) rectangle.
    const float thr = kBreakingThreshold;
    const float limX = thr + kPlanes[index].halfX;
    const float limY = thr + kPlanes[index].halfY;
    const bool miss = thr <= dist || vp.x < -limX || vp.y < -limY || limX < vp.x || limY < vp.y;
    if (!miss) {
        const Vec3 normal = {pl.basis.r[0].y * 0.0f + pl.basis.r[0].x * 0.0f + pl.basis.r[0].z,
                             pl.basis.r[1].y * 0.0f + pl.basis.r[1].x * 0.0f + pl.basis.r[1].z,
                             pl.basis.r[2].y * 0.0f + pl.basis.r[2].x * 0.0f + pl.basis.r[2].z};
        const Vec3 onFloor = xform(pl, Vec3{vp.x, vp.y, 0.0f});
        addContactPoint(manifold, cv, pl, normal, onFloor, dist);
    }
    if (manifold.count != 0)
        refreshContactPoints(manifold, cv, pl);
}

// Ball vs one arena triangle mesh: btConvexConcaveCollisionAlgorithm::processCollision
// (RVA 0x2928B0, the swapped [trimesh][sphere] entry; manifold body0 = ball, body1 = mesh).
void collideMesh(BallWorld& w, int meshIndex) {
    const ArenaMesh& mesh = w.arena->meshes[meshIndex];
    const Transform& cv = w.ball.xf;
    Manifold& manifold = w.manifolds[kNumPlanes + meshIndex];

    // btConvexTriangleCallback::setTimeStepAndCounters (RVA 0x2991F0): the sphere's box in mesh
    // space. The extra collision margin added to it is the mesh's, which is 0 (measured).
    const Transform inMesh = inverse(mesh.xf);
    const Vec3 c = xform(inMesh, cv.origin);
    // The ball shape's getAabb (vtable RVA 0x1A0D730 slot 1 = RVA 0x2A56F0) is not stock: the
    // extent is the radius plus the float 0.04 (RVA 0x1A0E388) added twice, so triangles up to
    // 0.08 beyond the sphere are still tested.
    const float extent = (kBallRadius + kSphereAabbMargin) + kSphereAabbMargin;
    const Vec3 aabbMin = {c.x - extent, c.y - extent, c.z - extent};
    const Vec3 aabbMax = {extent + c.x, extent + c.y, extent + c.z};

    forEachOverlappingLeaf(mesh, aabbMin, aabbMax, [&](int32_t tri) {
        Vec3 v[3];
        meshTriangle(mesh, tri, v);
        // btConvexTriangleCallback::processTriangle (RVA 0x296BA0): TestTriangleAgainstAabb2.
        const float lo[3] = {aabbMin.x, aabbMin.y, aabbMin.z};
        const float hi[3] = {aabbMax.x, aabbMax.y, aabbMax.z};
        const float t[3][3] = {
            {v[0].x, v[1].x, v[2].x}, {v[0].y, v[1].y, v[2].y}, {v[0].z, v[1].z, v[2].z}};
        for (int a = 0; a < 3; ++a) {
            const float mn = std::fmin(std::fmin(t[a][0], t[a][1]), t[a][2]);
            const float mx = std::fmax(std::fmax(t[a][0], t[a][1]), t[a][2]);
            if (mn > hi[a] || mx < lo[a])
                return;
        }
        // SphereTriangleDetector::getClosestPoints (RVA 0x28CC10).
        const Vec3 centre = invXform(mesh.xf, cv.origin);
        Vec3 point, normal;
        float depth;
        if (!sphereTriangle(v, centre, kBallRadius, kBreakingThreshold, point, normal, depth))
            return;
        const Vec3 normalWorld = mul(mesh.xf.basis, normal);
        const Vec3 pointWorld = xform(mesh.xf, point);
        const int idx = addContactPoint(manifold, cv, mesh.xf, normalWorld, pointWorld, depth);
        // Contact-added callback (RVA 0x415850), run on the manifold's copy of the point.
        adjustInternalEdge(manifold.pts[idx], mesh.xf, v, mesh.info[tri]);
    });
    if (manifold.count != 0)
        refreshContactPoints(manifold, cv, mesh.xf);
}

// ---- constraint solver -----------------------------------------------------------------------

// btSolverBody for the ball. The floor maps to the solver's shared "fixed body" whose
// m_originalBody is null: every term on that side is +0 and no impulse is applied to it.
struct SolverBody {
    Transform xf;
    Vec3 dLin, dAng;     // m_deltaLinearVelocity / m_deltaAngularVelocity
    Vec3 push, turn;     // split impulse pseudo-velocities
    Vec3 linVel, angVel; // velocities at solver start (after damping)
    Vec3 extForce;       // m_externalForceImpulse: gravity * invMass * dt
    Vec3 extTorque;      // m_externalTorqueImpulse (always zero here)
};

// btSolverConstraint, ball side only.
struct Row {
    Vec3 relposCrossNormal;
    Vec3 normal;
    Vec3 angComp;
    float appliedImpulse;
    float appliedPush;
    float friction;
    float jacDiagABInv;
    float rhs;
    float rhsPenetration;
    float lower;
    float upper;
    int frictionIndex;
    bool special; // Psyonix: skipped by velocity iterations, still used for split impulse
};

void fillJacobian(Row& row, const BallBody& b, const Vec3& relPos, const Vec3& axis) {
    const Vec3 torqueAxis = cross(relPos, axis);
    // invInertiaWorld * torqueAxis, times angular factor (1,1,1 measured at +0x390).
    row.angComp = mul(b.invInertiaWorld, torqueAxis);
    row.relposCrossNormal = torqueAxis;
    row.normal = axis;
    const Vec3 vec = cross(row.angComp, relPos);
    const float denom0 = dot(axis, vec) + kBallInvMass;
    const float denom1 = 0.0f; // fixed body
    row.jacDiagABInv = 1.0f / (denom1 + denom0);
}

// Velocity of the constraint along its jacobian, including the pending gravity impulse.
float jacobianVelocity(const Row& row, const SolverBody& sb) {
    const float vel1 = dot(row.normal, sb.linVel + sb.extForce) +
                       dot(row.relposCrossNormal, sb.angVel + sb.extTorque);
    const float vel2 = 0.0f; // fixed body: contactNormal2 and relpos2CrossNormal are zero
    return vel1 + vel2;
}

// setupContactConstraint (RVA 0x2BABF0). Psyonix differences from stock Bullet:
//  - restitution has no velocity threshold;
//  - a positive gap does NOT reduce the velocity error (the stock "- penetration / dt" term is
//    behind a flag at solverInfo+0x54 that is 0), so a contact with a gap is a hard stop.
void setupContactRow(Row& row, const BallBody& b, const SolverBody& sb, const Vec3& relPos,
                     const Vec3& normal, float distance, float restitutionCoef, float friction,
                     bool special) {
    fillJacobian(row, b, relPos, normal);
    const float penetration = distance + kLinearSlop;
    // Restitution uses the body's velocity WITHOUT the pending gravity impulse.
    const Vec3 vel = cross(b.angVel, relPos) + b.linVel;
    const float relVel = dot(normal, vel - Vec3{0.0f, 0.0f, 0.0f});
    float restitution = -relVel * restitutionCoef;
    if (!(0.0f <= restitution))
        restitution = 0.0f;
    row.friction = friction;
    row.appliedImpulse = 0.0f; // no warm starting (solverMode 0)
    row.appliedPush = 0.0f;
    const float velocityError = restitution - jacobianVelocity(row, sb);
    float positionalError = 0.0f;
    if (penetration <= 0.0f)
        positionalError = (-penetration * kErp2) / kDt;
    row.rhs = row.jacDiagABInv * velocityError;
    row.rhsPenetration = row.jacDiagABInv * positionalError;
    row.lower = 0.0f;
    row.upper = kContactUpperLimit;
    row.frictionIndex = -1;
    row.special = special;
}

// btPlaneSpace1, first tangent only.
Vec3 planeSpaceTangent(const Vec3& n) {
    if (std::fabs(n.z) > 0.7071067811865475244008443621048490f) {
        const float a = n.y * n.y + n.z * n.z;
        const float k = 1.0f / sqrtF(a);
        return {0.0f, -n.z * k, n.y * k};
    }
    const float a = n.x * n.x + n.y * n.y;
    const float k = 1.0f / sqrtF(a);
    return {-n.y * k, n.x * k, 0.0f};
}

// Friction direction and row for one contact (RVA 0x2BBE50 + 0x2BB760; stock Bullet 2.83 with
// one velocity-dependent friction direction, solverMode 0).
void setupFrictionRow(Row& row, const BallBody& b, const SolverBody& sb, const Vec3& relPos,
                      const Vec3& normal, float friction, int contactIndex) {
    // getVelocityInLocalPointNoDelta: includes the external impulses.
    const Vec3 vel = (sb.linVel + sb.extForce) + cross(sb.angVel + sb.extTorque, relPos);
    const float relVel = dot(normal, vel);
    Vec3 dir = vel - normal * relVel;
    const float lat = length2(dir);
    if (lat > kSimdEpsilon) {
        const float inv = 1.0f / sqrtF(lat);
        dir = dir * inv;
    } else {
        dir = planeSpaceTangent(normal);
    }
    fillJacobian(row, b, relPos, dir);
    row.friction = friction;
    row.appliedImpulse = 0.0f;
    row.appliedPush = 0.0f;
    const float vel1 =
        dot(row.normal, sb.linVel + sb.extForce) + dot(row.relposCrossNormal, sb.angVel);
    const float relVel2 = vel1 + 0.0f;
    const float velocityError = 0.0f - relVel2;
    row.rhs = velocityError * row.jacDiagABInv;
    row.rhsPenetration = 0.0f;
    row.lower = -friction;
    row.upper = friction;
    row.frictionIndex = contactIndex;
    row.special = false;
}

void applyImpulse(Vec3& lin, Vec3& ang, const Row& row, float delta) {
    // internalApplyImpulse: linear factor and angular factor are (1,1,1).
    lin = {kBallInvMass * row.normal.x * delta + lin.x, kBallInvMass * row.normal.y * delta + lin.y,
           kBallInvMass * row.normal.z * delta + lin.z};
    ang = {delta * row.angComp.x + ang.x, delta * row.angComp.y + ang.y,
           delta * row.angComp.z + ang.z};
}

// resolveSingleConstraintRowLowerLimit (RVA 0x2B91B0) and ...RowGeneric (RVA 0x2B8ED0).
// cfm is 0; the fixed body's terms are +0.
void solveRow(Row& row, SolverBody& sb, bool upperLimit) {
    float delta = row.rhs - row.appliedImpulse * 0.0f;
    const float dv1 = dot(row.relposCrossNormal, sb.dAng) + dot(row.normal, sb.dLin);
    const float dv2 = 0.0f;
    delta = delta - dv1 * row.jacDiagABInv;
    delta = delta - dv2 * row.jacDiagABInv;
    const float sum = row.appliedImpulse + delta;
    if (sum < row.lower) {
        delta = row.lower - row.appliedImpulse;
        row.appliedImpulse = row.lower;
    } else if (upperLimit && sum > row.upper) {
        delta = row.upper - row.appliedImpulse;
        row.appliedImpulse = row.upper;
    } else {
        row.appliedImpulse = sum;
    }
    applyImpulse(sb.dLin, sb.dAng, row, delta);
}

// resolveSplitPenetrationImpulse (RVA 0x2B9470).
void solveSplitRow(Row& row, SolverBody& sb) {
    if (row.rhsPenetration == 0.0f)
        return;
    float delta = row.rhsPenetration - row.appliedPush * 0.0f;
    const float dv1 = dot(row.relposCrossNormal, sb.turn) + dot(row.normal, sb.push);
    const float dv2 = 0.0f;
    delta = delta - dv1 * row.jacDiagABInv;
    delta = delta - dv2 * row.jacDiagABInv;
    const float sum = row.appliedPush + delta;
    if (sum < row.lower) {
        delta = row.lower - row.appliedPush;
        row.appliedPush = row.lower;
    } else {
        row.appliedPush = sum;
    }
    applyImpulse(sb.push, sb.turn, row, delta);
}

// solveGroup (RVA 0x2BD0F0): setup 0x2BE070, iterations 0x2BDF70, finish 0x2BD1C0.
void solve(BallWorld& w, const Vec3& totalForce) {
    BallBody& b = w.ball;

    // initSolverBody (RVA 0x2B6540).
    SolverBody sb;
    sb.xf = b.xf;
    sb.dLin = sb.dAng = sb.push = sb.turn = {0.0f, 0.0f, 0.0f};
    sb.linVel = b.linVel;
    sb.angVel = b.angVel;
    sb.extForce = {totalForce.x * kBallInvMass * kDt, totalForce.y * kBallInvMass * kDt,
                   totalForce.z * kBallInvMass * kDt};
    sb.extTorque = {0.0f, 0.0f, 0.0f}; // no torque is ever applied to the ball here

    // convertContacts (RVA 0x2B2C40 -> convertContact 0x2B2550).
    // Psyonix "special" contacts: every ball-vs-world manifold point is flagged special
    // (manifold point +0x10C). Each still gets a contact row, but that row is used only for the
    // split-impulse (penetration recovery) pass. For the velocity pass, the points of a body are
    // accumulated (count, normal, friction, restitution, |rel_pos|) at body+0x250.. and replaced
    // by ONE averaged contact with distance 0 (convertContactSpecial, RVA 0x2B21C0).
    Row contacts[4 * kNumManifolds + 1];
    int numContacts = 0;
    int specialCount = 0;
    Vec3 normalSum = {0.0f, 0.0f, 0.0f};
    float frictionSum = 0.0f;
    float restitutionSum = 0.0f;
    float distSum = 0.0f;
    for (int mi = 0; mi < kNumManifolds; ++mi) {
        for (int i = 0; i < w.manifolds[mi].count; ++i) {
            const ContactPoint& p = w.manifolds[mi].pts[i];
            const Vec3 relPos = p.worldA - b.xf.origin;
            setupContactRow(contacts[numContacts++], b, sb, relPos, p.normal, p.distance,
                            kBallRestitution, kBallFriction, true);
            ++specialCount;
            frictionSum = frictionSum + kBallFriction;
            restitutionSum = restitutionSum + kBallRestitution;
            normalSum = normalSum + p.normal;
            distSum =
                distSum + sqrtF(relPos.y * relPos.y + relPos.x * relPos.x + relPos.z * relPos.z);
        }
    }

    Row frictionRow{};
    bool hasFriction = false;
    if (specialCount > 0) {
        if (specialCount > 1) {
            const float inv = 1.0f / static_cast<float>(specialCount);
            frictionSum = frictionSum * inv;
            restitutionSum = restitutionSum * inv;
            const float nInv = 1.0f / sqrtF(length2(normalSum));
            normalSum = normalSum * nInv;
            distSum = distSum * inv;
        }
        const Vec3 relPos = {-normalSum.x * distSum, -normalSum.y * distSum,
                             -normalSum.z * distSum};
        const int idx = numContacts++;
        setupContactRow(contacts[idx], b, sb, relPos, normalSum, 0.0f, restitutionSum, frictionSum,
                        false);
        setupFrictionRow(frictionRow, b, sb, relPos, normalSum, frictionSum, idx);
        hasFriction = true;
    }

    // Iterations (RVA 0x2BDF70): split-impulse pass first (0x2BF880, every contact row), then
    // the velocity pass (0x2BFBD0): contacts in order, then friction limited by its contact.
    for (int it = 0; it < kNumIterations; ++it) {
        for (int i = 0; i < numContacts; ++i)
            solveSplitRow(contacts[i], sb);
    }
    for (int it = 0; it < kNumIterations; ++it) {
        for (int i = 0; i < numContacts; ++i) {
            if (!contacts[i].special)
                solveRow(contacts[i], sb, false);
        }
        if (hasFriction) {
            const float total = contacts[frictionRow.frictionIndex].appliedImpulse;
            if (total > 0.0f) {
                frictionRow.lower = -(frictionRow.friction * total);
                frictionRow.upper = frictionRow.friction * total;
                solveRow(frictionRow, sb, true);
            }
        }
    }

    // Finish (RVA 0x2BD1C0): write back velocities, apply split impulse to the transform.
    sb.linVel = sb.linVel + sb.dLin;
    sb.angVel = sb.angVel + sb.dAng;
    if (!isZero(sb.push) || !isZero(sb.turn)) {
        const Vec3 turn = {sb.turn.x * kSplitImpulseTurnErp, sb.turn.y * kSplitImpulseTurnErp,
                           sb.turn.z * kSplitImpulseTurnErp};
        sb.xf = integrateTransform(sb.xf, sb.push, turn, kDt);
    }
    b.linVel = sb.linVel + sb.extForce;
    b.angVel = sb.angVel + sb.extTorque;
    b.xf = sb.xf;
}

// ---- world step ------------------------------------------------------------------------------

// btDiscreteDynamicsWorld::updateActivationState (RVA 0x2C12D0, stock).
void updateActivationState(BallBody& b) {
    if (b.activation != kIslandSleeping) {
        if (kLinSleepThreshold * kLinSleepThreshold <= length2(b.linVel) ||
            kAngSleepThreshold * kAngSleepThreshold <= length2(b.angVel)) {
            b.deactivationTime = 0.0f;
            b.activation = 0;
        } else {
            b.deactivationTime = kDt + b.deactivationTime;
        }
    }
    const bool wantsSleeping = b.activation == kIslandSleeping ||
                               b.activation == kWantsDeactivation ||
                               b.deactivationTime > kDeactivationTime;
    if (!wantsSleeping) {
        b.activation = kActive;
    } else {
        if (b.activation == kActive)
            b.activation = kWantsDeactivation;
        if (b.activation == kIslandSleeping) {
            b.angVel = {0.0f, 0.0f, 0.0f};
            b.linVel = {0.0f, 0.0f, 0.0f};
        }
    }
}

// World step: Bullet world vtable slot 39 (RVA 0x2B7970), one fixed sub-step of dt.
void worldStep(BallWorld& w) {
    BallBody& b = w.ball;

    // applyGravity (RVA 0x2C27E0): only active bodies accumulate the gravity force.
    Vec3 totalForce = {0.0f, 0.0f, 0.0f};
    if (!isSleeping(b))
        totalForce = totalForce + Vec3{0.0f, 0.0f, kGravityForceZ};

    // predictUnconstraintMotion (RVA 0x2B7A90) -> applyDamping (RVA 0x2C21D0).
    // Psyonix clamps the speed first, then stock damping. The gravity impulse is NOT applied
    // here (Bullet 2.83 style): it rides along as the solver's external force impulse.
    clampSpeed(b.linVel, kBodyMaxLinSpeed);
    clampSpeed(b.angVel, kBodyMaxAngSpeed);
    b.linVel = {kLinDampFactor * b.linVel.x, kLinDampFactor * b.linVel.y,
                kLinDampFactor * b.linVel.z};
    b.angVel = {1.0f * b.angVel.x, 1.0f * b.angVel.y, 1.0f * b.angVel.z};

    // createPredictiveContacts (RVA 0x2B2CB0) and the CCD clamp in integrateTransforms sweep a
    // sphere of the body's CCD swept radius, which is 0 for the ball (measured). They can only
    // trigger when the ball's centre itself crosses a surface within one tick; not modelled.

    // performDiscreteCollisionDetection (RVA 0x279850).
    for (int i = 0; i < kNumPlanes; ++i)
        collidePlane(w, i);
    if (w.arena != nullptr) {
        for (int i = 0; i < w.arena->numMeshes; ++i)
            collideMesh(w, i);
    }

    // Simulation islands (RVA 0x2B1890, stock btSimulationIslandManager::buildIslands): the ball
    // is alone in its island (the floor is static).
    if (b.activation != kActive) {
        b.activation = kIslandSleeping;
    }

    // solveConstraints (RVA 0x2BC7A0): sleeping islands are skipped.
    if (!isSleeping(b))
        solve(w, totalForce);

    // integrateTransforms (RVA 0x2B7470): active bodies only; proceedToTransform refreshes the
    // world inertia tensor.
    if (!isSleeping(b)) {
        b.xf = integrateTransform(b.xf, b.linVel, b.angVel, kDt);
        b.invInertiaWorld = ballInvInertiaWorld(b.xf.basis);
    }

    updateActivationState(b);
}

void fnv(uint64_t& h, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        h ^= (v >> (8 * i)) & 0xffu;
        h *= 0x100000001b3ull;
    }
}

void fnvVec(uint64_t& h, const Vec3& v) {
    fnv(h, floatBits(v.x));
    fnv(h, floatBits(v.y));
    fnv(h, floatBits(v.z));
}

} // namespace

Mat3 ballInvInertiaWorld(const Mat3& basis) {
    // m_worldTransform.getBasis().scaled(m_invInertiaLocal) *
    // m_worldTransform.getBasis().transpose()
    Mat3 scaled;
    for (int i = 0; i < 3; ++i) {
        scaled.r[i] = {basis.r[i].x * kInvInertiaLocal, basis.r[i].y * kInvInertiaLocal,
                       basis.r[i].z * kInvInertiaLocal};
    }
    return mul(scaled, transpose(basis));
}

RBState ballRBState(const BallBody& b) {
    // RVA 0xEAAE40.
    RBState s;
    s.pos = {b.xf.origin.x * kBUToUU, b.xf.origin.y * kBUToUU, b.xf.origin.z * kBUToUU};
    s.rot = getRotation(b.xf.basis);
    s.linVel = {b.linVel.x * kBUToUU, b.linVel.y * kBUToUU, b.linVel.z * kBUToUU};
    s.angVel = b.angVel;
    s.sleeping = isSleeping(b);
    if (!s.sleeping) {
        clampRBVel(s.linVel, kMaxLinSpeedUU);
        clampRBVel(s.angVel, kMaxAngSpeed);
    } else {
        s.linVel = {0.0f, 0.0f, 0.0f};
        s.angVel = {0.0f, 0.0f, 0.0f};
    }
    return s;
}

void initBallWorld(BallWorld& w, const Transform& xf, const Vec3& linVel, const Vec3& angVel,
                   const ArenaCollision* arena) {
    w.arena = arena;
    w.ball.xf = xf;
    w.ball.linVel = linVel;
    w.ball.angVel = angVel;
    w.ball.invInertiaWorld = ballInvInertiaWorld(xf.basis);
    w.ball.activation = kActive;
    w.ball.deactivationTime = 0.0f;
    for (Manifold& m : w.manifolds)
        m = Manifold{};
    w.tick = 0;
}

void stepBallWorld(BallWorld& w) {
    // Tick function RVA 0xEEB3F0: pre-step callbacks, world step, post-step callbacks.
    preStep(w.ball);
    worldStep(w);
    ++w.tick;
}

uint64_t hashBallWorld(const BallWorld& w) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const Vec3& r : w.ball.xf.basis.r)
        fnvVec(h, r);
    fnvVec(h, w.ball.xf.origin);
    fnvVec(h, w.ball.linVel);
    fnvVec(h, w.ball.angVel);
    fnv(h, static_cast<uint32_t>(w.ball.activation));
    fnv(h, static_cast<uint32_t>(w.tick & 0xffffffffu));
    fnv(h, static_cast<uint32_t>(w.tick >> 32));
    return h;
}

} // namespace rlios::sim
