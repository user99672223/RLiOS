// Contact manifold data shared by the collision and solver code.
#pragma once

#include "sim/vec.h"

#include <cstdint>

namespace rlios::sim {

// btManifoldPoint, reduced to the fields that influence the simulation.
struct ContactPoint {
    Vec3 localA; // on the ball, ball space
    Vec3 localB; // on the static object, in its space
    Vec3 worldA;
    Vec3 worldB;
    Vec3 normal; // world, on B, pointing at the ball
    float distance;
};

// btPersistentManifold (ball = body 0, static object = body 1).
struct Manifold {
    int32_t count;
    ContactPoint pts[4];
};

} // namespace rlios::sim
