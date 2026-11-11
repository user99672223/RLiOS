// The game's replicated rigid body state ("RBState") and its per-tick quantization.
//
// Evidence (RocketLeague.exe, RVAs relative to image base 0x140000000):
// Every physics tick, BEFORE the Bullet step, the ball's pre-step callback (RVA 0xED9F70 ->
// 0xEDB230) builds an RBState from the Bullet body (0xEAAE40), pushes it through the network
// serializer and back (0xDFF0B0 -> 0xDF6F80, archive version 12), and writes the quantized result
// back into the Bullet body (0xEF0300). So the physics state itself is quantized once per tick.
// Units: RBState is in Unreal units (uu); Bullet units (BU) are uu * 0.02.
#pragma once

#include "sim/vec.h"

namespace rlios::sim {

struct RBState {
    Quat rot;
    Vec3 pos;    // uu
    Vec3 linVel; // uu/s
    Vec3 angVel; // rad/s
    bool sleeping;
};

// Position: fixed point, 0.01 uu steps, round to nearest (RVA 0x388190).
float quantizePos(float uu);
// Velocities: truncate to 1/scale steps, then nudge 10% of a step away from zero (RVA 0x388260).
// scale is 100 for linear velocity and 10000 for angular velocity.
float quantizeVel(float v, float scale);
// Rotation: "smallest three" with 18 bits per component (RVA 0x3837C0/0x37B1A0 write,
// 0x3879A0 read).
Quat quantizeQuat(const Quat& q);

// Full serializer round trip (RVA 0xDF6F80, version 12).
void quantize(RBState& s);

} // namespace rlios::sim
