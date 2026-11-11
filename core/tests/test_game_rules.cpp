// Unit tests for the soccar rules (core/game/match.*) against a scripted physics.
#include "game/arena.h"
#include "game/match.h"
#include "game/rules.h"
#include "test.h"

using namespace rlios::game;
using rlios::sim::Vec3;

namespace {

// Physics whose bodies the test places by hand; step() only reports scripted events.
class FakePhysics final : public Physics {
  public:
    void setCarCount(int n) override { n_ = n; }
    void placeCar(int i, const Vec3& pos, int32_t yaw) override {
        cars[i] = CarBody{};
        cars[i].pos = pos;
        yaws[i] = yaw;
        ++placed[i];
    }
    void setCarActive(int i, bool a) override { active[i] = a; }
    void placeBall(const Vec3& pos) override { ballBody = BallBody{pos, {0, 0, 0}}; }
    void step(const CarControls*, const uint8_t* boostActive, StepEvents& ev) override {
        ++steps;
        for (int i = 0; i < n_; ++i)
            boosted[i] = boostActive[i];
        ev = StepEvents{};
        ev.ballTouchMask = touchMask;
        ev.numHits = numHits;
        for (int h = 0; h < numHits; ++h)
            ev.hits[h] = hits[h];
        touchMask = 0;
        numHits = 0;
    }
    CarBody car(int i) const override { return cars[i]; }
    BallBody ball() const override { return ballBody; }
    uint64_t hash() const override { return 0; }

    int n_ = 0;
    CarBody cars[kMaxCars] = {};
    int32_t yaws[kMaxCars] = {};
    int placed[kMaxCars] = {};
    bool active[kMaxCars] = {};
    uint8_t boosted[kMaxCars] = {};
    BallBody ballBody = {};
    uint32_t touchMask = 0;
    int numHits = 0;
    CarHit hits[4] = {};
    int steps = 0;
};

CarControls none[kMaxCars] = {};

void runTicks(Match& m, int n, const CarControls* c = none) {
    for (int i = 0; i < n; ++i)
        m.tick(c);
}

constexpr int kCountdown = kCountdownSeconds * 120;

void testSetBoostAmount() {
    CHECK_EQ(setBoostAmount(0.5f, 9999.0f), 1.0f);
    CHECK_EQ(setBoostAmount(0.5f, -1.0f), 0.0f);
    CHECK_EQ(setBoostAmount(0.5f, 0.62f), 0.62f);
    // Changes below 1e-8 are dropped (RVA 0xEEFD40), except a set to exactly 0.
    CHECK_EQ(setBoostAmount(0.5f, 0.5f + 5.96e-8f), 0.5f + 5.96e-8f);
    CHECK_EQ(setBoostAmount(1e-9f, 0.0f), 0.0f);
    CHECK_EQ(setBoostAmount(0.0f, 5e-9f), 0.0f);
}

void testKickoffSpawns() {
    for (int n = 1; n <= 4; ++n) {
        FakePhysics p;
        MatchConfig cfg;
        cfg.blueCars = n;
        cfg.orangeCars = n;
        cfg.seed = 7 + static_cast<uint64_t>(n);
        Match m(cfg, p);
        CHECK(m.state().phase == Phase::Countdown);
        for (int i = 0; i < n; ++i) {
            // Orange car i mirrors blue car i (symmetric kickoff), all on kickoff spots.
            const CarBody b = p.cars[i];
            const CarBody o = p.cars[n + i];
            CHECK_EQ(o.pos.x, -b.pos.x);
            CHECK_EQ(o.pos.y, -b.pos.y);
            CHECK_EQ(p.yaws[n + i], (p.yaws[i] + 32768) & 65535);
            CHECK(b.pos.y < 0.0f);
            bool onSpot = false;
            for (const StartSpec& s : kKickoffStarts)
                onSpot = onSpot || (s.pos.x == b.pos.x && s.pos.y == b.pos.y);
            CHECK(onSpot);
            for (int j = 0; j < i; ++j) // distinct spots within a team (n <= 5)
                CHECK(!(p.cars[j].pos.x == b.pos.x && p.cars[j].pos.y == b.pos.y));
            CHECK_EQ(m.state().players[i].boost, kStartBoost);
        }
        CHECK_EQ(p.ballBody.pos.x, 0.0f);
        CHECK_EQ(p.ballBody.pos.y, 0.0f);
    }
}

void testCountdownFreezes() {
    FakePhysics p;
    Match m(MatchConfig{}, p);
    runTicks(m, kCountdown - 1);
    CHECK(m.state().phase == Phase::Countdown);
    CHECK_EQ(p.steps, 0);
    runTicks(m, 1);
    CHECK(m.state().phase == Phase::Active);
    runTicks(m, 1);
    CHECK_EQ(p.steps, 1);
    CHECK_EQ(m.displaySeconds(), 300);
}

void testClockStartsOnTouchOrDelay() {
    {
        FakePhysics p;
        Match m(MatchConfig{}, p);
        runTicks(m, kCountdown + 100);
        CHECK(!m.state().clockRunning);
        CHECK_EQ(m.state().clockTicks, 300 * 120);
        p.touchMask = 1;
        runTicks(m, 1);
        CHECK(m.state().clockRunning);
        CHECK_EQ(m.state().lastTouch, 0);
        CHECK_EQ(m.state().players[0].touches, 1);
        runTicks(m, 120);
        CHECK_EQ(m.displaySeconds(), 299);
    }
    {
        FakePhysics p;
        Match m(MatchConfig{}, p);
        runTicks(m, kCountdown + 5 * 120 - 1);
        CHECK(!m.state().clockRunning);
        runTicks(m, 1);
        CHECK(m.state().clockRunning);
    }
}

void testBoostConsumptionAndMinTime() {
    FakePhysics p;
    Match m(MatchConfig{}, p);
    runTicks(m, kCountdown);
    CarControls c[kMaxCars] = {};
    c[0].boost = 1;
    m.tick(c);
    CHECK_EQ(p.boosted[0], 1);
    CHECK_EQ(p.boosted[1], 0);
    const float expect =
        setBoostAmount(kStartBoost, kStartBoost - kBoostPerSecond * (1.0f / 120.0f));
    CHECK_EQ(m.state().players[0].boost, expect);
    // Released after 1 tick: still boosts until MinBoostTime (0.1 s = 12 ticks).
    for (int t = 0; t < 11; ++t) {
        m.tick(none);
        CHECK_EQ(p.boosted[0], 1);
    }
    m.tick(none);
    CHECK_EQ(p.boosted[0], 0);
    // Runs dry: about 1 s of boost from a third of a tank (rate 1/3 per second).
    int ticks = 0;
    while (m.state().players[0].boost > 0.0f && ticks < 1000) {
        m.tick(c);
        ++ticks;
    }
    CHECK(ticks > 100 && ticks < 120);
    m.tick(c);
    CHECK_EQ(p.boosted[0], 0);
}

void testBoostPads() {
    FakePhysics p;
    Match m(MatchConfig{}, p);
    runTicks(m, kCountdown);
    // Small pad _15 at (2048, -1036, 64.08): r=128.
    int small = -1, big = -1;
    for (int k = 0; k < kNumPads; ++k) {
        if (small < 0 && !kStadiumPads[k].big)
            small = k;
        if (big < 0 && kStadiumPads[k].big)
            big = k;
    }
    const Vec3 sp = kStadiumPads[small].pos;
    p.cars[0].pos = {sp.x + 127.0f, sp.y, 17.0f};
    p.cars[1].pos = {0.0f, 0.0f, 17.0f};
    m.tick(none);
    CHECK_EQ(m.state().players[0].boost, kStartBoost + kSmallPadAmount);
    CHECK_EQ(m.state().pads[small].cooldownTicks, 4 * 120);
    // Picked-up pad is unavailable until its RespawnDelay passes.
    runTicks(m, 4 * 120);
    CHECK_EQ(m.state().players[0].boost, kStartBoost + kSmallPadAmount);
    CHECK_EQ(m.state().pads[small].cooldownTicks, 0);
    m.tick(none);
    CHECK_EQ(m.state().players[0].boost, kStartBoost + kSmallPadAmount + kSmallPadAmount);
    // Just outside the cylinder radius: no pickup.
    p.cars[0].pos = {sp.x + 128.0f, sp.y, 17.0f};
    runTicks(m, 4 * 120 + 1);
    CHECK_EQ(m.state().pads[small].cooldownTicks, 0);
    // Big pad fills the tank (9999 clamped to max) and respawns in 10 s.
    p.cars[0].pos = kStadiumPads[big].pos;
    m.tick(none);
    CHECK_EQ(m.state().players[0].boost, kMaxBoost);
    CHECK_EQ(m.state().pads[big].cooldownTicks, 10 * 120);
    // A full car does not take a pad.
    p.cars[0].pos = {sp.x, sp.y, 17.0f};
    m.tick(none);
    CHECK_EQ(m.state().pads[small].cooldownTicks, 0);
}

void testDemolition() {
    FakePhysics p;
    MatchConfig cfg;
    cfg.blueCars = 2;
    cfg.orangeCars = 1;
    Match m(cfg, p);
    runTicks(m, kCountdown);
    // Not supersonic: bump only.
    p.hits[0] = {0, 2};
    p.numHits = 1;
    m.tick(none);
    CHECK(!m.state().players[2].demolished);
    // Teammate: never.
    p.cars[0].supersonic = 1;
    p.hits[0] = {0, 1};
    p.numHits = 1;
    m.tick(none);
    CHECK(!m.state().players[1].demolished);
    // Supersonic opponent: demolished, removed from physics, respawns after 3 s with start boost.
    m.mutableState().players[2].boost = 0.9f;
    const int placedBefore = p.placed[2];
    p.hits[0] = {0, 2};
    p.numHits = 1;
    m.tick(none);
    CHECK(m.state().players[2].demolished);
    CHECK(!p.active[2]);
    CHECK_EQ(m.state().players[0].demos, 1);
    runTicks(m, 3 * 120 - 1);
    CHECK(m.state().players[2].demolished);
    m.tick(none);
    CHECK(!m.state().players[2].demolished);
    CHECK(p.active[2]);
    CHECK_EQ(p.placed[2], placedBefore + 1);
    CHECK(p.cars[2].pos.y > 4000.0f); // orange respawn spots mirror blue's (y = -4608)
    CHECK_EQ(m.state().players[2].boost, kStartBoost);
}

void testGoalAndReset() {
    FakePhysics p;
    Match m(MatchConfig{}, p);
    runTicks(m, kCountdown);
    p.touchMask = 1;
    runTicks(m, 1);
    // On the line: not yet (whole ball must cross).
    p.ballBody.pos = {0.0f, kGoalLineY + kBallRadius, 300.0f};
    m.tick(none);
    CHECK(m.state().phase == Phase::Active);
    // Outside the goal mouth width: no goal.
    p.ballBody.pos = {kGoalHalfWidth, 5300.0f, 300.0f};
    m.tick(none);
    CHECK(m.state().phase == Phase::Active);
    p.ballBody.pos = {0.0f, 5300.0f, 300.0f};
    m.tick(none);
    CHECK(m.state().phase == Phase::PostGoal);
    CHECK_EQ(m.state().score[0], 1);
    CHECK_EQ(m.state().score[1], 0);
    CHECK_EQ(m.state().players[0].goals, 1);
    const int32_t clock = m.state().clockTicks;
    runTicks(m, 3 * 120 - 1);
    CHECK(m.state().phase == Phase::PostGoal);
    CHECK_EQ(m.state().clockTicks, clock); // clock stopped
    m.tick(none);
    CHECK(m.state().phase == Phase::Countdown);
    CHECK_EQ(p.ballBody.pos.y, 0.0f);
    // Orange scores in the blue goal (-y).
    runTicks(m, kCountdown);
    p.ballBody.pos = {0.0f, -5300.0f, 100.0f};
    m.tick(none);
    CHECK_EQ(m.state().score[1], 1);
}

void testRegulationEndAndOvertime() {
    FakePhysics p;
    MatchConfig cfg;
    cfg.gameSeconds = 2;
    Match m(cfg, p);
    runTicks(m, kCountdown);
    p.touchMask = 1;
    p.ballBody.pos = {0.0f, 0.0f, 500.0f}; // in the air
    runTicks(m, 2 * 120);
    CHECK_EQ(m.state().clockTicks, 0);
    CHECK(m.state().phase == Phase::Active); // waits for the ball to land
    p.ballBody.pos = {0.0f, 0.0f, kBallRadius};
    m.tick(none);
    // Tied 0-0: overtime with a kickoff.
    CHECK(m.state().overtime);
    CHECK(m.state().phase == Phase::Countdown);
    runTicks(m, kCountdown);
    p.touchMask = 1;
    runTicks(m, 240);
    CHECK_EQ(m.state().overtimeTicks, 240);
    p.ballBody.pos = {0.0f, -5300.0f, 100.0f};
    m.tick(none);
    CHECK(m.state().phase == Phase::Ended);
    CHECK_EQ(m.state().winner, 1);
    const uint64_t t = m.state().tick;
    m.tick(none);
    CHECK_EQ(m.state().tick, t);
}

void testRegulationWin() {
    FakePhysics p;
    MatchConfig cfg;
    cfg.gameSeconds = 1;
    Match m(cfg, p);
    runTicks(m, kCountdown);
    p.touchMask = 1;
    runTicks(m, 1);
    p.ballBody.pos = {0.0f, 5300.0f, 100.0f};
    m.tick(none);
    runTicks(m, 3 * 120 + kCountdown);
    p.touchMask = 1;
    p.ballBody.pos = {0.0f, 0.0f, kBallRadius};
    runTicks(m, 120);
    CHECK(m.state().phase == Phase::Ended);
    CHECK_EQ(m.state().winner, 0);
    CHECK(!m.state().overtime);
}

} // namespace

int main() {
    testSetBoostAmount();
    testKickoffSpawns();
    testCountdownFreezes();
    testClockStartsOnTouchOrDelay();
    testBoostConsumptionAndMinTime();
    testBoostPads();
    testDemolition();
    testGoalAndReset();
    testRegulationEndAndOvertime();
    testRegulationWin();
    TEST_MAIN_RETURN();
}
