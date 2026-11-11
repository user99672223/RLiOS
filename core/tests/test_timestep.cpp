#include "sim/fixed_timestep.h"
#include "sim/state.h"
#include "test.h"

using namespace rlios::sim;

namespace {
// Run the sim for totalNs of real time split into frames of frameNs; returns final state hash.
uint64_t runFrames(uint64_t totalNs, uint64_t frameNs, uint64_t* ticksOut) {
    FixedTimestep ts(120, 1u << 20);
    State s = initialState();
    uint64_t elapsed = 0;
    while (elapsed < totalNs) {
        const uint64_t dt = (totalNs - elapsed < frameNs) ? totalNs - elapsed : frameNs;
        elapsed += dt;
        const uint32_t n = ts.advance(dt);
        for (uint32_t i = 0; i < n; ++i)
            step(s, Input{0.25f, -0.5f});
    }
    *ticksOut = s.tick;
    return hashState(s);
}
} // namespace

int main() {
    // Same results at any frame rate: tick count is exactly floor(T*120/1e9).
    const uint64_t total = 10ull * 1000000000ull + 123456789ull;
    const uint64_t expectTicks = total * 120 / 1000000000ull;
    uint64_t t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0;
    const uint64_t h0 = runFrames(total, 1000000ull, &t0);   // 1000 fps
    const uint64_t h1 = runFrames(total, 16666667ull, &t1);  // 60 fps
    const uint64_t h2 = runFrames(total, 33333333ull, &t2);  // 30 fps
    const uint64_t h3 = runFrames(total, 6944444ull, &t3);   // 144 fps
    const uint64_t h4 = runFrames(total, 3ull * total, &t4); // one giant frame
    CHECK_EQ(t0, expectTicks);
    CHECK_EQ(t1, expectTicks);
    CHECK_EQ(t2, expectTicks);
    CHECK_EQ(t3, expectTicks);
    CHECK_EQ(t4, expectTicks);
    CHECK_EQ(h0, h1);
    CHECK_EQ(h0, h2);
    CHECK_EQ(h0, h3);
    CHECK_EQ(h0, h4);

    // Exactly 120 ticks per second, no drift over an hour of 7 ms frames.
    {
        FixedTimestep ts(120, 1u << 20);
        uint64_t ticks = 0;
        for (uint64_t i = 0; i < 3600ull * 1000000000ull / 7000000ull; ++i)
            ticks += ts.advance(7000000ull);
        const uint64_t frames = 3600ull * 1000000000ull / 7000000ull;
        CHECK_EQ(ticks, frames * 7000000ull * 120 / 1000000000ull);
    }

    // Catch-up cap: a huge stall runs at most `cap` ticks and counts the rest as dropped.
    {
        FixedTimestep ts(120, 16);
        const uint32_t n = ts.advance(10ull * 1000000000ull); // 1200 ticks owed
        CHECK_EQ(n, 16u);
        CHECK_EQ(ts.droppedTicks(), 1200u - 16u);
        CHECK_EQ(ts.advance(0), 0u); // no spiral: nothing left over
    }

    // Time until next tick.
    {
        FixedTimestep ts(120, 16);
        CHECK_EQ(ts.nsUntilNextTick(), 8333334u); // ceil(1e9/120)
        ts.advance(1000000ull);
        CHECK(ts.nsUntilNextTick() < 8333334u);
    }
    CHECK_EQ(tickTimeNs(120), 1000000000ull);
    TEST_MAIN_RETURN();
}
