#include "game/match.h"

#include "game/hash.h"
#include "game/rules.h"

namespace rlios::game {

namespace {
using sim::Vec3;
constexpr float kDt = 1.0f / 120.0f;
constexpr int32_t secondsToTicks(float s) {
    return static_cast<int32_t>(s * 120.0f + 0.5f);
}
constexpr int32_t kMinBoostTicks = secondsToTicks(kMinBoostTime);
// [RS] BALL_REST_Z = 93.15 (kickoff ball height), UNPROVEN.
constexpr float kBallSpawnZ = 93.15f;

uint64_t splitmix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
} // namespace

float setBoostAmount(float current, float requested) {
    float v = requested;
    if (!(0.0f <= v))
        v = 0.0f;
    else if (kMaxBoost <= v)
        v = kMaxBoost;
    const float d = v - current;
    const float ad = d < 0.0f ? -d : d;
    if (sim::bitsFloat(kBoostSetEpsilonBits) <= ad || v == 0.0f)
        return v;
    return current;
}

Match::Match(const MatchConfig& cfg, Physics& phys) : cfg_(cfg), phys_(phys), s_{}, ev_{} {
    int n = cfg.blueCars + cfg.orangeCars;
    if (n > kMaxCars)
        n = kMaxCars;
    s_.numCars = n;
    s_.rng = splitmix(cfg.seed);
    s_.winner = kNoTeam;
    s_.clockTicks = cfg.gameSeconds * kTicksPerSecond;
    for (int i = 0; i < n; ++i)
        s_.players[i].team = i < cfg.blueCars ? 0 : 1;
    phys_.setCarCount(n);
    kickoff();
}

uint32_t Match::nextRand() {
    // xorshift64*: our own deterministic generator. The game's kickoff RNG is not identified yet.
    uint64_t x = s_.rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    s_.rng = x;
    return static_cast<uint32_t>((x * 0x2545f4914f6cdd1dull) >> 32);
}

void Match::kickoff() {
    // Kickoff spots: a seeded shuffle of the five primary PlayerStart_TA spots; orange gets the
    // same draw mirrored (kickoffs are symmetric). Selection algorithm UNPROVEN.
    int order[kNumKickoffStarts] = {0, 1, 2, 3, 4};
    for (int i = kNumKickoffStarts - 1; i > 0; --i) {
        const int j = static_cast<int>(nextRand() % static_cast<uint32_t>(i + 1));
        const int t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    int slot[2] = {0, 0};
    for (int i = 0; i < s_.numCars; ++i) {
        PlayerState& p = s_.players[i];
        const int k = order[slot[p.team]++ % kNumKickoffStarts];
        const StartSpec st = p.team == 0 ? kKickoffStarts[k] : mirrorForOrange(kKickoffStarts[k]);
        phys_.placeCar(i, st.pos, st.yawUU);
        phys_.setCarActive(i, true);
        p.demolished = 0;
        p.boosting = 0;
        p.boostTicks = 0;
        p.respawnTicks = 0;
        p.boost = setBoostAmount(p.boost, kStartBoost);
    }
    for (PadState& pad : s_.pads)
        pad.cooldownTicks = 0; // pads reset on kickoff (UNPROVEN; RocketSim does the same)
    phys_.placeBall({0.0f, 0.0f, kBallSpawnZ});
    s_.phase = Phase::Countdown;
    s_.phaseTicks = kCountdownSeconds * kTicksPerSecond;
    s_.clockRunning = 0;
    s_.lastTouch = -1;
}

void Match::demolish(int victim) {
    PlayerState& p = s_.players[victim];
    p.demolished = 1;
    p.boosting = 0;
    p.respawnTicks = secondsToTicks(kDemoRespawnSeconds);
    phys_.setCarActive(victim, false);
}

void Match::respawn(int i) {
    PlayerState& p = s_.players[i];
    // GameEvent_Soccar_TA.bAllowDemoSpawnSelection [CDO] lets humans pick; bots get a seeded spot.
    const int k = static_cast<int>(nextRand() % kNumRespawnStarts);
    const StartSpec st = p.team == 0 ? kRespawnStarts[k] : mirrorForOrange(kRespawnStarts[k]);
    phys_.placeCar(i, st.pos, st.yawUU);
    phys_.setCarActive(i, true);
    p.demolished = 0;
    p.boostTicks = 0;
    p.boost = setBoostAmount(p.boost, kStartBoost);
}

void Match::updatePads() {
    for (int k = 0; k < kNumPads; ++k) {
        PadState& pad = s_.pads[k];
        if (pad.cooldownTicks > 0) {
            --pad.cooldownTicks;
            continue;
        }
        const PadSpec& spec = kStadiumPads[k];
        const float r = spec.big ? kBigPadRadius : kSmallPadRadius;
        // Car origin inside the pad's CylinderComponent (UNPROVEN: the game uses actor touch
        // between the car's collision and the cylinder, VehiclePickup_TA.IsTouchingAVehicle
        // RVA 0xECC250). First car in index order wins (UNPROVEN).
        for (int i = 0; i < s_.numCars; ++i) {
            PlayerState& p = s_.players[i];
            if (p.demolished || p.boost >= kMaxBoost) // CanPickup: not when full (UNPROVEN)
                continue;
            const CarBody c = phys_.car(i);
            const float dx = c.pos.x - spec.pos.x;
            const float dy = c.pos.y - spec.pos.y;
            const float dz = c.pos.z - spec.pos.z;
            if (dx * dx + dy * dy >= r * r || dz >= kPadHeight || dz <= -kPadHeight)
                continue;
            const float amount = spec.big ? kBigPadAmount : kSmallPadAmount;
            p.boost = setBoostAmount(p.boost, p.boost + amount);
            pad.cooldownTicks = secondsToTicks(spec.big ? kBigPadRespawn : kSmallPadRespawn);
            break;
        }
    }
}

bool Match::checkGoal() {
    const BallBody b = phys_.ball();
    const float ay = b.pos.y < 0.0f ? -b.pos.y : b.pos.y;
    if (ay <= kGoalLineY + kBallRadius)
        return false;
    if (b.pos.x >= kGoalHalfWidth || b.pos.x <= -kGoalHalfWidth || b.pos.z <= kGoalMinZ ||
        b.pos.z >= kGoalMaxZ)
        return false;
    // Ball in the +y goal (orange's, Goal_TA TeamNum 1) scores for blue.
    scoreGoal(b.pos.y > 0.0f ? 0 : 1);
    return true;
}

void Match::scoreGoal(int team) {
    ++s_.score[team];
    ++s_.goals;
    if (s_.lastTouch >= 0 && s_.players[s_.lastTouch].team == team)
        ++s_.players[s_.lastTouch].goals;
    if (s_.overtime) { // next goal wins
        s_.phase = Phase::Ended;
        s_.winner = static_cast<uint8_t>(team);
        return;
    }
    s_.phase = Phase::PostGoal;
    s_.phaseTicks = secondsToTicks(kPostGoalSeconds);
}

void Match::endRegulationOrOvertime() {
    if (s_.score[0] != s_.score[1]) {
        s_.phase = Phase::Ended;
        s_.winner = s_.score[0] > s_.score[1] ? 0 : 1;
        return;
    }
    s_.overtime = 1;
    kickoff();
}

int32_t Match::displaySeconds() const {
    return (s_.clockTicks + kTicksPerSecond - 1) / kTicksPerSecond;
}

void Match::tick(const CarControls* controls) {
    if (s_.phase == Phase::Ended)
        return;
    ++s_.tick;
    if (s_.phase == Phase::Countdown) {
        if (--s_.phaseTicks <= 0) {
            s_.phase = Phase::Active;
            s_.phaseTicks = 0;
        }
        return; // cars and ball frozen
    }

    // Boost decision (CarComponent_Boost_TA): needs boost left; once started it runs for at
    // least MinBoostTime even if the button is released.
    uint8_t boostActive[kMaxCars] = {};
    for (int i = 0; i < s_.numCars; ++i) {
        PlayerState& p = s_.players[i];
        if (p.demolished) {
            p.boosting = 0;
            continue;
        }
        const bool held = controls[i].boost != 0;
        if (!p.boosting && held && p.boost > 0.0f) {
            p.boosting = 1;
            p.boostTicks = 0;
        } else if (p.boosting && !held && p.boostTicks >= kMinBoostTicks) {
            p.boosting = 0;
        }
        if (p.boosting && p.boost <= 0.0f)
            p.boosting = 0;
        if (p.boosting) {
            p.boost = setBoostAmount(p.boost, p.boost - kBoostPerSecond * kDt);
            ++p.boostTicks;
            boostActive[i] = 1;
        }
    }

    phys_.step(controls, boostActive, ev_);

    for (int i = 0; i < s_.numCars; ++i) {
        if (ev_.ballTouchMask & (1u << i)) {
            s_.lastTouch = i;
            ++s_.players[i].touches;
        }
    }
    // Respawns first, so a car demolished this tick waits the full respawn time.
    for (int i = 0; i < s_.numCars; ++i) {
        PlayerState& p = s_.players[i];
        if (p.demolished && --p.respawnTicks <= 0)
            respawn(i);
    }
    // Demolitions: an opposing, supersonic attacker hitting with its front ([CDO] Car_TA
    // DemolishTarget/DemolishSpeed, see rules.h).
    for (int h = 0; h < ev_.numHits; ++h) {
        const CarHit hit = ev_.hits[h];
        PlayerState& a = s_.players[hit.attacker];
        PlayerState& v = s_.players[hit.victim];
        if (a.demolished || v.demolished || a.team == v.team)
            continue;
        if (!phys_.car(hit.attacker).supersonic)
            continue;
        demolish(hit.victim);
        ++a.demos;
    }
    updatePads();

    if (s_.phase == Phase::PostGoal) {
        if (--s_.phaseTicks <= 0) {
            if (s_.clockTicks == 0 && !cfg_.unlimitedTime)
                endRegulationOrOvertime(); // goal at 0:00 decides it, or ties it: overtime
            else
                kickoff();
        }
        return;
    }

    // Active.
    ++s_.phaseTicks;
    if (checkGoal())
        return;
    if (!s_.clockRunning &&
        (ev_.ballTouchMask != 0 || s_.phaseTicks >= secondsToTicks(kBallHasBeenHitStartDelay)))
        s_.clockRunning = 1;
    if (!s_.clockRunning || cfg_.unlimitedTime)
        return;
    if (s_.overtime) {
        ++s_.overtimeTicks;
        return;
    }
    if (s_.clockTicks > 0)
        --s_.clockTicks;
    if (s_.clockTicks == 0 &&
        phys_.ball().pos.z <= kBallRadius + kBallGroundSlack) // waits for the ball to land
        endRegulationOrOvertime();
}

uint64_t Match::hash() const {
    Fnv h;
    h.u32(static_cast<uint32_t>(s_.phase));
    h.u32(s_.overtime);
    h.u32(s_.clockRunning);
    h.u32(s_.winner);
    h.u64(s_.tick);
    h.u32(static_cast<uint32_t>(s_.score[0]));
    h.u32(static_cast<uint32_t>(s_.score[1]));
    h.u32(static_cast<uint32_t>(s_.clockTicks));
    h.u32(static_cast<uint32_t>(s_.overtimeTicks));
    h.u32(static_cast<uint32_t>(s_.phaseTicks));
    h.u64(s_.rng);
    for (int i = 0; i < s_.numCars; ++i) {
        const PlayerState& p = s_.players[i];
        h.u32(p.demolished | (p.boosting << 8));
        h.f(p.boost);
        h.u32(static_cast<uint32_t>(p.boostTicks));
        h.u32(static_cast<uint32_t>(p.respawnTicks));
    }
    for (const PadState& pad : s_.pads)
        h.u32(static_cast<uint32_t>(pad.cooldownTicks));
    h.u64(phys_.hash());
    return h.v;
}

} // namespace rlios::game
