// Fixed-timestep accumulator (CLAUDE.md section 4, "Time").
// Pure integer arithmetic: the accumulator counts in units of (nanoseconds * tickHz) so that
// the number of ticks after T ns is exactly floor(T * tickHz / 1e9), independent of how T is
// split into frames. Catch-up is capped to avoid a spiral of death; dropped ticks are counted.
#pragma once

#include <cstdint>

namespace rlios::sim {

class FixedTimestep {
  public:
    static constexpr uint64_t kNsPerSec = 1000000000ull;

    explicit FixedTimestep(uint32_t hz = 120, uint32_t maxCatchUpTicks = 16)
        : hz_(hz), cap_(maxCatchUpTicks) {}

    // Feed elapsed real time; returns how many ticks to run now (<= cap).
    uint32_t advance(uint64_t dtNs) {
        acc_ += dtNs * hz_;
        uint64_t n = acc_ / kNsPerSec;
        acc_ -= n * kNsPerSec;
        if (n > cap_) {
            dropped_ += n - cap_;
            n = cap_;
        }
        return static_cast<uint32_t>(n);
    }

    // Real time until the next tick is due.
    uint64_t nsUntilNextTick() const { return (kNsPerSec - acc_ + hz_ - 1) / hz_; }

    uint64_t droppedTicks() const { return dropped_; }
    uint32_t hz() const { return hz_; }

  private:
    uint32_t hz_;
    uint32_t cap_;
    uint64_t acc_ = 0;
    uint64_t dropped_ = 0;
};

// Monotonic time of tick k relative to the sim clock origin (exact: k * 1e9 / 120).
inline uint64_t tickTimeNs(uint64_t tick, uint32_t hz = 120) {
    return tick * FixedTimestep::kNsPerSec / hz;
}

} // namespace rlios::sim
