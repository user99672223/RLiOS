// The ball sim must end on the hash that the exactness harness proved against the real game.
// Each scenario below was compared by tools/harness/ball_harness with the game's recording, bit
// for bit on every tick, and the final hash printed there is pinned here. Reproducing it (CI:
// x86-64 and arm64) shows both architectures still compute exactly what the game computed,
// without the recording (which is never committed).
#include "sim/ball_world.h"
#include "test.h"

using namespace rlios::sim;

namespace {
// Identity rotation; position and velocities in Bullet units (1 BU = 50 uu).
uint64_t run(Vec3 pos, Vec3 vel, Vec3 angVel, int ticks) {
    BallWorld w;
    initBallWorld(w, Transform{identityMat3(), pos}, vel, angVel);
    for (int i = 0; i < ticks; ++i)
        stepBallWorld(w);
    std::printf("ball hash %016llx\n", static_cast<unsigned long long>(hashBallWorld(w)));
    return hashBallWorld(w);
}
} // namespace

int main() {
    // ball_roll_a: floor, horizontal velocity + spin.
    CHECK_EQ(run({0.0f, -20.0f, 1.9f}, {9.5f, 3.25f, 0.0f}, {1.5f, -2.25f, 3.0f}, 1200),
             0x34ea6971627ca524ull);
    // ball_wall_px: +X side wall hit at mid height, then floor.
    CHECK_EQ(run({70.0f, 10.0f, 20.0f}, {40.0f, 5.0f, 3.0f}, {2.0f, 1.0f, -3.0f}, 1200),
             0xe009329cfa979ae0ull);
    // ball_ceiling: ceiling hit, then floor.
    CHECK_EQ(run({10.0f, 20.0f, 30.0f}, {4.0f, -3.0f, 45.0f}, {5.0f, 0.0f, 2.0f}, 1200),
             0xadfb330bf121c6d3ull);
    TEST_MAIN_RETURN();
}
