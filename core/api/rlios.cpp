#include "api/rlios.h"

#include <new>

#include "game/session.h"

struct RliosSim {
    rlios::game::Session session;
    explicit RliosSim(const rlios::game::SessionConfig& c) : session(c) {}
};

extern "C" {

uint64_t rlios_now_ns(void) {
    return rlios::game::monotonicNowNs();
}

RliosSim* rlios_create(const RliosConfig* cfg) {
    rlios::game::SessionConfig sc;
    if (cfg) {
        sc.manual = cfg->manual_stepping != 0;
        if (cfg->max_catch_up_ticks)
            sc.maxCatchUp = cfg->max_catch_up_ticks;
    }
    return new (std::nothrow) RliosSim(sc);
}

void rlios_destroy(RliosSim* sim) {
    delete sim;
}
int rlios_start(RliosSim* sim) {
    return sim && sim->session.start() ? 1 : 0;
}
void rlios_stop(RliosSim* sim) {
    if (sim)
        sim->session.stop();
}
void rlios_set_input(RliosSim* sim, float ax, float ay) {
    if (sim)
        sim->session.setInput(ax, ay);
}
void rlios_step(RliosSim* sim, uint32_t ticks) {
    if (sim)
        sim->session.step(ticks);
}

int rlios_get_render_state(RliosSim* sim, uint64_t now_ns, RliosRenderState* out) {
    rlios::game::RenderState rs;
    if (!sim || !out || !sim->session.renderState(now_ns, rs))
        return 0;
    out->tick = rs.tick;
    out->alpha = rs.alpha;
    out->x = rs.x;
    out->y = rs.y;
    out->vx = rs.vx;
    out->vy = rs.vy;
    out->state_hash = rs.hash;
    return 1;
}

void rlios_get_diagnostics(const RliosSim* sim, RliosDiagnostics* out) {
    if (!sim || !out)
        return;
    const rlios::game::Diagnostics d = sim->session.diagnostics();
    out->tick = d.tick;
    out->state_hash = d.hash;
    out->dropped_ticks = d.droppedTicks;
    out->tick_rate_hz = d.tickRateHz;
    out->running = d.running;
}

} // extern "C"
