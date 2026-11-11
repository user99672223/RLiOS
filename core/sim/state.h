// PLACEHOLDER simulation state. This is NOT Rocket League physics.
// It exists to exercise the tick loop, snapshot plumbing and hashing. Real physics arrives in
// later milestones and must trace to evidence (see ../CLAUDE.md section 1).
#pragma once

#include "sim/vec.h"

#include <cstdint>

namespace rlios::sim {

struct State {
    float x;
    float y;
    float vx;
    float vy;
    uint64_t tick;
};

struct Input {
    float ax;
    float ay;
};

// Fixed simulation rate. Time is owned by the sim: 120 ticks per second.
constexpr uint32_t kTickHz = 120;
constexpr float kDt = 1.0f / 120.0f;

State initialState();

// Advance one tick. Only +,-,*,/ on float (IEEE exact); no transcendental functions.
void step(State& s, const Input& in);

// FNV-1a 64 over the little-endian bytes of the float BIT PATTERNS (never epsilons).
uint64_t hashState(const State& s);

} // namespace rlios::sim
