#include <cinttypes>
#include <cstdio>
#include <string>

#include "game/scenario.h"
#include "game/session.h"
#include "sim/state.h"
#include "test.h"

using namespace rlios;

namespace {
const char* kScenario = "ticks 1000\n"
                        "input 0 0 0\n"
                        "input 100 1.5 -0.5\n"
                        "input 400 0x1p-1 0x1.8p0\n"
                        "input 700 0 0\n";
}

int main() {
    // Hash is over bit patterns: -0.0 and +0.0 must differ, NaN payloads must be distinguishable.
    {
        sim::State a = sim::initialState();
        sim::State b = a;
        a.x = 0.0f;
        b.x = -0.0f;
        CHECK(sim::hashState(a) != sim::hashState(b));
    }

    game::Scenario sc;
    std::string err;
    CHECK(game::parseScenario(kScenario, sc, err));
    CHECK_EQ(sc.ticks, 1000u);
    CHECK_EQ(sc.inputs.size(), 4u);
    CHECK(!game::parseScenario("input 1 2 3\n", sc, err)); // missing ticks
    CHECK(!game::parseScenario("ticks 1\nbogus\n", sc, err));
    CHECK(game::parseScenario(kScenario, sc, err));

    // Two runs are bit-identical.
    uint64_t h1 = 0, h2 = 0;
    game::runScenario(sc, [&](const sim::State& s) { h1 = sim::hashState(s); });
    game::runScenario(sc, [&](const sim::State& s) { h2 = sim::hashState(s); });
    CHECK_EQ(h1, h2);
    // Golden: recorded from this very code; must be identical on x86-64 and arm64 (CI checks the
    // full per-tick dump as well). Changing sim math changes this on purpose.
    std::printf("scenario final hash = %016" PRIx64 "\n", h1);
    CHECK_EQ(h1, 0x989de8e435daf471ull);

    // Session in manual mode (the path the C API uses) matches the scenario runner.
    {
        game::SessionConfig cfg;
        cfg.manual = true;
        game::Session s(cfg);
        s.setInput(0.0f, 0.0f);
        s.step(100);
        s.setInput(1.5f, -0.5f);
        s.step(300);
        s.setInput(sim::bitsFloat(0x3f000000u), sim::bitsFloat(0x3fc00000u)); // 0.5, 1.5
        s.step(300);
        s.setInput(0.0f, 0.0f);
        s.step(300);
        CHECK_EQ(s.diagnostics().tick, 1000u);
        CHECK_EQ(s.diagnostics().hash, h1);
        game::RenderState rs;
        CHECK(s.renderState(0, rs));
        CHECK_EQ(rs.tick, 1000u);
        CHECK_EQ(rs.hash, h1);
    }

    // Live thread: starts, ticks near 120 Hz, stops; render state interpolation stays finite and
    // within [prev, curr].
    {
        game::Session s{game::SessionConfig{}};
        CHECK(s.start());
        const uint64_t t0 = game::monotonicNowNs();
        game::RenderState rs{};
        while (game::monotonicNowNs() - t0 < 500000000ull) {
            CHECK(s.renderState(game::monotonicNowNs(), rs));
            CHECK(rs.alpha >= 0.0 && rs.alpha <= 1.0);
            CHECK(rs.x == rs.x && rs.y == rs.y); // not NaN
        }
        s.stop();
        const uint64_t ticks = s.diagnostics().tick;
        // 0.5 s => ~60 ticks; allow wide margin for loaded CI machines.
        CHECK(ticks >= 20 && ticks <= 62);
    }
    TEST_MAIN_RETURN();
}
