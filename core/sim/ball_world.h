// Ball on the DFH Stadium floor: the part of Rocket League's physics tick needed for milestone 3.
// One sphere rigid body, the floor's bounded plane, the game's per-tick state quantization, and
// Psyonix's modified Bullet contact solver. See ball_world.cpp for the evidence behind each step.
#pragma once

#include "sim/rb_state.h"
#include "sim/vec.h"

#include <cstdint>

namespace rlios::sim {

// btCollisionObject activation states.
enum Activation : int32_t {
    kActive = 1,
    kIslandSleeping = 2,
    kWantsDeactivation = 3,
};

struct BallBody {
    Transform xf; // world transform, Bullet units (1 BU = 50 uu)
    Vec3 linVel;  // BU/s
    Vec3 angVel;  // rad/s
    // btRigidBody::m_invInertiaTensorWorld. It is real state: it is refreshed only when the body
    // is integrated, from the un-quantized basis, and used by the next tick's solver.
    Mat3 invInertiaWorld;
    int32_t activation;
    float deactivationTime;
};

// btManifoldPoint, reduced to the fields that influence the simulation.
struct ContactPoint {
    Vec3 localA; // on the ball, ball space
    Vec3 localB; // on the floor, floor space
    Vec3 worldA;
    Vec3 worldB;
    Vec3 normal; // world, on B (floor), pointing at the ball
    float distance;
};

// btPersistentManifold (ball = body 0, floor = body 1).
struct Manifold {
    int32_t count;
    ContactPoint pts[4];
};

struct BallWorld {
    BallBody ball;
    Manifold floor;
    uint64_t tick;
};

// Ball at rest pose `xf` with the given velocities, awake, no contacts.
void initBallWorld(BallWorld& w, const Transform& xf, const Vec3& linVel, const Vec3& angVel);

// btRigidBody::updateInertiaTensor for the ball: basis.scaled(invInertiaLocal) * basis^T.
Mat3 ballInvInertiaWorld(const Mat3& basis);

// One 120 Hz physics tick: pre-step quantization, then the Bullet world step.
void stepBallWorld(BallWorld& w);

// The RBState the game would build from the body right now (before quantization).
RBState ballRBState(const BallBody& b);

// FNV-1a 64 over the float bit patterns of the body state (never epsilons).
uint64_t hashBallWorld(const BallWorld& w);

} // namespace rlios::sim
