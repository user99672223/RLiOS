// A whole bot-vs-bot soccar match on the placeholder physics: what --soak and the golden tests run.
#pragma once

#include <cstdint>

#include "game/bot.h"
#include "game/match.h"
#include "game/stub_physics.h"

namespace rlios::game {

class BotMatch {
  public:
    explicit BotMatch(const MatchConfig& cfg) : match_(cfg, phys_), bots_(cfg.seed) {}
    void step(uint32_t ticks) {
        for (uint32_t t = 0; t < ticks && !ended(); ++t) {
            bots_.compute(match_, phys_, controls_);
            match_.tick(controls_);
        }
    }
    bool ended() const { return match_.state().phase == Phase::Ended; }
    const Match& match() const { return match_; }
    const StubPhysics& physics() const { return phys_; }

  private:
    StubPhysics phys_; // declared before match_: Match keeps a reference
    Match match_;
    BotInputs bots_;
    CarControls controls_[kMaxCars] = {};
};

} // namespace rlios::game
