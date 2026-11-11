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
    TEST_MAIN_RETURN();
}
