#include "api/rlios.h"
#include "test.h"

int main() {
    RliosConfig cfg{1, 0};
    RliosSim* sim = rlios_create(&cfg);
    CHECK(sim != nullptr);
    CHECK(rlios_start(sim) == 0); // manual mode has no thread
    rlios_set_input(sim, 1.0f, 0.0f);
    rlios_step(sim, 240);
    RliosDiagnostics d{};
    rlios_get_diagnostics(sim, &d);
    CHECK(d.tick == 240);
    RliosRenderState rs{};
    CHECK(rlios_get_render_state(sim, rlios_now_ns(), &rs) == 1);
    CHECK(rs.tick == 240);
    CHECK(rs.alpha == 1.0);
    CHECK(rs.state_hash == d.state_hash);
    rlios_destroy(sim);

    RliosConfig live{0, 0};
    sim = rlios_create(&live);
    CHECK(rlios_start(sim) == 1);
    rlios_stop(sim);
    rlios_destroy(sim);

    // Bot-vs-bot match through the C API: runs to the end, deterministic.
    RliosMatchConfig mc{2, 2, 2};
    RliosMatch* m = rlios_match_create(&mc);
    CHECK(m != nullptr);
    int ended = 0;
    for (int i = 0; i < 1000 && !ended; ++i)
        ended = rlios_match_step(m, 120);
    CHECK(ended == 1);
    RliosMatchInfo info{};
    rlios_match_get_info(m, &info);
    CHECK(info.phase == 3);
    CHECK(info.winner == 0 || info.winner == 1);
    CHECK(info.num_cars == 4);
    CHECK(info.total_goals == static_cast<uint32_t>(info.score_blue + info.score_orange));
    RliosCarInfo car{};
    CHECK(rlios_match_get_car(m, 3, &car) == 1);
    CHECK(car.team == 1);
    CHECK(rlios_match_get_car(m, 4, &car) == 0);
    rlios_match_destroy(m);
    RliosMatchConfig bad{0, 1, 1};
    CHECK(rlios_match_create(&bad) == nullptr);
    TEST_MAIN_RETURN();
}
