// Soccar match rules: kickoff countdown, clock, goals, overtime, match end, boost accounting,
// boost pads, demolitions. Drives a game::Physics. Fixed 120 Hz; all timers are integer tick
// counts (our representation; the game's own float timers are not reproduced yet).
// Tick order (UNPROVEN, to be checked against the game's actor tick groups):
//   phase timers -> boost decision -> physics step -> touches -> respawns -> demolitions
//   -> boost pads -> goal -> clock.
#pragma once

#include <cstdint>

#include "game/arena.h"
#include "game/physics.h"

namespace rlios::game {

enum class Phase : uint8_t { Countdown = 0, Active = 1, PostGoal = 2, Ended = 3 };

inline constexpr int32_t kTicksPerSecond = 120;
inline constexpr uint8_t kNoTeam = 255;

struct MatchConfig {
    int32_t blueCars = 1;
    int32_t orangeCars = 1;
    uint64_t seed = 1;
    int32_t gameSeconds = 300; // rules.h kGameTimeSeconds
    bool unlimitedTime = false;
};

struct PlayerState {
    uint8_t team; // 0 blue (defends -y), 1 orange
    uint8_t demolished;
    uint8_t boosting;
    uint8_t pad;
    float boost;          // 0..kMaxBoost
    int32_t boostTicks;   // ticks since boosting started (min boost time)
    int32_t respawnTicks; // demolished: ticks until respawn
    int32_t goals, demos, touches;
};

struct PadState {
    int32_t cooldownTicks; // 0: available
};

struct MatchState {
    Phase phase;
    uint8_t overtime;
    uint8_t clockRunning;
    uint8_t winner; // kNoTeam until Ended
    uint64_t tick;
    int32_t score[2];
    int32_t clockTicks;    // regulation time remaining
    int32_t overtimeTicks; // time played in overtime
    int32_t phaseTicks;    // ticks left in Countdown / PostGoal; ticks since Go in Active
    int32_t numCars;
    int32_t lastTouch; // car index, -1 none
    int32_t goals;     // total goals (kickoff counter)
    uint64_t rng;
    PlayerState players[kMaxCars];
    PadState pads[kNumPads];
};

// Boost set with CarComponent_Boost_TA.SetBoostAmount semantics (RVA 0xEEFD40).
float setBoostAmount(float current, float requested);

class Match {
  public:
    Match(const MatchConfig& cfg, Physics& phys);
    // One 120 Hz tick. `controls` has numCars entries (ignored while frozen).
    void tick(const CarControls* controls);
    const MatchState& state() const { return s_; }
    MatchState& mutableState() { return s_; } // tests only
    Physics& physics() { return phys_; }
    uint64_t hash() const;
    // Seconds shown on the scoreboard (ceil of remaining regulation time).
    int32_t displaySeconds() const;
    // Exposed for tests.
    void kickoff();
    void demolish(int victim);

  private:
    uint32_t nextRand();
    void respawn(int i);
    void updatePads();
    bool checkGoal();
    void scoreGoal(int team);
    void endRegulationOrOvertime();

    MatchConfig cfg_;
    Physics& phys_;
    MatchState s_;
    StepEvents ev_;
};

} // namespace rlios::game
