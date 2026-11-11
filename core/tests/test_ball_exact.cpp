// The ball sim must end on the hash that the exactness harness proved against the real game.
// Scenario "ball_roll_a": ball dropped 3.75 uu above the floor with horizontal velocity and
// spin, identity rotation, 1200 ticks. tools/harness/ball_harness compared all 1200 ticks with
// the game's recording bit for bit and printed this final hash; reproducing it here (CI: x86-64
// and arm64) shows both architectures still compute exactly what the game computed, without the
// recording (which is never committed).
#include "sim/ball_world.h"
#include "test.h"

using namespace rlios::sim;

int main() {
    BallWorld w;
    const Transform xf{identityMat3(), {0.0f, -20.0f, 1.9f}};
    initBallWorld(w, xf, {9.5f, 3.25f, 0.0f}, {1.5f, -2.25f, 3.0f});
    for (int i = 0; i < 1200; ++i)
        stepBallWorld(w);
    std::printf("ball hash %016llx\n", static_cast<unsigned long long>(hashBallWorld(w)));
    CHECK_EQ(hashBallWorld(w), 0x34ea6971627ca524ull);
    TEST_MAIN_RETURN();
}
