// Ball in DFH Stadium: the part of Rocket League's physics tick needed for the ball. One sphere
// rigid body, the arena's six bounded planes (built in) and its triangle meshes (data, optional),
// the game's per-tick state quantization, and Psyonix's modified Bullet contact solver. See
// ball_world.cpp for the evidence behind each step.
#pragma once

#include "sim/arena_mesh.h"
#include "sim/contact.h"
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

// Stadium_P bounded planes: 2 goal floors, floor, ceiling, 2 side walls.
inline constexpr int kNumPlanes = 6;

inline constexpr int kNumManifolds = kNumPlanes + kMaxArenaMeshes;

struct BallWorld {
    BallBody ball;
    // One per static object, Bullet world object order: the planes, then the arena meshes.
    Manifold manifolds[kNumManifolds];
    // Triangle meshes of the arena (corners, ramps, goals, back walls). Null: planes only. The
    // data is extracted from the game and never part of the repo; the caller keeps it alive.
    const ArenaCollision* arena;
    uint64_t tick;
};

// Ball at pose `xf` with the given velocities, awake, no contacts.
void initBallWorld(BallWorld& w, const Transform& xf, const Vec3& linVel, const Vec3& angVel,
                   const ArenaCollision* arena = nullptr);

// btRigidBody::updateInertiaTensor for the ball: basis.scaled(invInertiaLocal) * basis^T.
Mat3 ballInvInertiaWorld(const Mat3& basis);

// One 120 Hz physics tick: pre-step quantization, then the Bullet world step.
void stepBallWorld(BallWorld& w);

// The RBState the game would build from the body right now (before quantization).
RBState ballRBState(const BallBody& b);

// FNV-1a 64 over the float bit patterns of the body state (never epsilons).
uint64_t hashBallWorld(const BallWorld& w);

} // namespace rlios::sim
