// Headless scenario: tick count + per-tick inputs, as text.
//   # comment
//   ticks <N>
//   input <tick> <ax> <ay>     (applies from <tick> onward; floats, decimal or hex-float)
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "sim/state.h"

namespace rlios::game {

struct InputEvent {
    uint64_t tick;
    sim::Input input;
};

struct Scenario {
    uint64_t ticks = 0;
    std::vector<InputEvent> inputs; // must be sorted by tick
};

bool parseScenario(const std::string& text, Scenario& out, std::string& err);

// Runs the scenario; calls onTick(state) for the initial state and after every tick.
void runScenario(const Scenario& sc, const std::function<void(const sim::State&)>& onTick);

} // namespace rlios::game
