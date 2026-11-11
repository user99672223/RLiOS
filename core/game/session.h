// Game session: owns time (fixed 120 Hz), the sim state, and snapshot publication.
// PLACEHOLDER content: the sim it drives is not Rocket League physics yet.
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "sim/fixed_timestep.h"
#include "sim/state.h"
#include "sim/triple_buffer.h"

namespace rlios::game {

// Immutable snapshot published after every tick. Carries the previous tick's state so a
// consumer that skipped ticks can still interpolate between two adjacent states.
struct Snapshot {
    uint32_t valid;
    uint32_t pad;
    sim::State prev;
    sim::State curr;
    uint64_t hash;       // hashState(curr)
    uint64_t currTimeNs; // absolute monotonic time at which `curr` is due (live mode)
};

struct RenderState {
    uint64_t tick;
    double alpha;
    float x, y;
    float vx, vy;
    uint64_t hash;
};

struct Diagnostics {
    uint64_t tick;
    uint64_t hash;
    uint64_t droppedTicks;
    double tickRateHz;
    uint32_t running;
};

struct SessionConfig {
    bool manual = false; // manual: no thread; caller drives step()
    uint32_t maxCatchUp = 16;
};

uint64_t monotonicNowNs();

class Session {
  public:
    explicit Session(const SessionConfig& cfg);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool start();
    void stop();
    void setInput(float ax, float ay);
    // Manual mode only: run n ticks synchronously.
    void step(uint32_t n);

    // Consumer side (single consumer thread): interpolate the two latest states for `nowNs`.
    bool renderState(uint64_t nowNs, RenderState& out);
    Diagnostics diagnostics() const;

  private:
    void tickOnce();
    void threadMain();

    SessionConfig cfg_;
    sim::State state_;
    sim::TripleBuffer<Snapshot> buffer_;
    sim::FixedTimestep ts_;
    std::atomic<uint64_t> input_;
    std::atomic<bool> stopFlag_{false};
    std::atomic<uint32_t> running_{0};
    std::thread thread_;
    uint64_t startNs_ = 0;

    std::atomic<uint64_t> tick_{0};
    std::atomic<uint64_t> hash_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<double> rate_{0.0};
};

} // namespace rlios::game
