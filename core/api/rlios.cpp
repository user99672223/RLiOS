#include "api/rlios.h"

#include <new>

#include "game/bot_match.h"
#include "game/session.h"

struct RliosSim {
    rlios::game::Session session;
    explicit RliosSim(const rlios::game::SessionConfig& c) : session(c) {}
};

struct RliosMatch {
    rlios::game::BotMatch match;
    explicit RliosMatch(const rlios::game::MatchConfig& c) : match(c) {}
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

RliosMatch* rlios_match_create(const RliosMatchConfig* cfg) {
    rlios::game::MatchConfig mc;
    if (cfg) {
        mc.blueCars = static_cast<int32_t>(cfg->blue_cars);
        mc.orangeCars = static_cast<int32_t>(cfg->orange_cars);
        mc.seed = cfg->seed;
    }
    if (mc.blueCars < 1 || mc.orangeCars < 1 || mc.blueCars > 4 || mc.orangeCars > 4)
        return nullptr;
    return new (std::nothrow) RliosMatch(mc);
}

void rlios_match_destroy(RliosMatch* m) {
    delete m;
}

int rlios_match_step(RliosMatch* m, uint32_t ticks) {
    if (!m)
        return 0;
    m->match.step(ticks);
    return m->match.ended() ? 1 : 0;
}

void rlios_match_get_info(const RliosMatch* m, RliosMatchInfo* out) {
    if (!m || !out)
        return;
    const rlios::game::Match& mt = m->match.match();
    const rlios::game::MatchState& s = mt.state();
    *out = RliosMatchInfo{};
    out->tick = s.tick;
    out->phase = static_cast<uint32_t>(s.phase);
    out->overtime = s.overtime;
    out->score_blue = s.score[0];
    out->score_orange = s.score[1];
    out->seconds_remaining = mt.displaySeconds();
    out->winner = s.winner == rlios::game::kNoTeam ? -1 : s.winner;
    out->num_cars = static_cast<uint32_t>(s.numCars);
    for (int i = 0; i < s.numCars; ++i)
        out->total_demos += static_cast<uint32_t>(s.players[i].demos);
    out->total_goals = static_cast<uint32_t>(s.goals);
    const rlios::game::BallBody b = m->match.physics().ball();
    out->ball_x = b.pos.x;
    out->ball_y = b.pos.y;
    out->ball_z = b.pos.z;
    out->state_hash = mt.hash();
}

int rlios_match_get_car(const RliosMatch* m, uint32_t index, RliosCarInfo* out) {
    if (!m || !out)
        return 0;
    const rlios::game::MatchState& s = m->match.match().state();
    if (index >= static_cast<uint32_t>(s.numCars))
        return 0;
    const rlios::game::CarBody c = m->match.physics().car(static_cast<int>(index));
    const rlios::game::PlayerState& p = s.players[index];
    out->x = c.pos.x;
    out->y = c.pos.y;
    out->z = c.pos.z;
    out->boost = p.boost;
    out->team = p.team;
    out->demolished = p.demolished;
    return 1;
}

} // extern "C"
