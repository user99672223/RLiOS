// Deterministic bot input source for soak and golden tests (not the game's AI): chases a point
// behind the ball on the line to the opponent goal, boosts on long straight runs, with seeded
// jitter so matches differ per seed. Only + - * / sqrt: identical on x86-64 and arm64.
#pragma once

#include <cstdint>

#include "game/match.h"

namespace rlios::game {

class BotInputs {
  public:
    explicit BotInputs(uint64_t seed) : rng_(seed * 0x9e3779b97f4a7c15ull + 1) {}
    // Fills controls[0..numCars).
    void compute(const Match& m, Physics& phys, CarControls* controls);

  private:
    uint32_t next();
    uint64_t rng_;
    float jitter_[kMaxCars] = {};
};

} // namespace rlios::game
