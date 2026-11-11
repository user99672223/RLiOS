// PLACEHOLDER: damped-free spring "point in a box" integrator (semi-implicit Euler).
#include "sim/state.h"

namespace rlios::sim {

namespace {
constexpr float kSpring = 4.0f;
constexpr float kInputGain = 2.0f;
constexpr float kBound = 1.0f;

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;

uint64_t fnvU32(uint64_t h, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        h ^= static_cast<uint64_t>((v >> (8 * i)) & 0xffu);
        h *= kFnvPrime;
    }
    return h;
}
} // namespace

State initialState() {
    return State{0.6f, 0.0f, 0.0f, 0.8f, 0};
}

void step(State& s, const Input& in) {
    // Order of operations is fixed and fp-contract is off for sim/, so no fused multiply-add.
    const float ax = in.ax * kInputGain - s.x * kSpring;
    const float ay = in.ay * kInputGain - s.y * kSpring;
    s.vx = s.vx + ax * kDt;
    s.vy = s.vy + ay * kDt;
    s.x = s.x + s.vx * kDt;
    s.y = s.y + s.vy * kDt;
    if (s.x > kBound) {
        s.x = kBound;
        s.vx = -s.vx;
    } else if (s.x < -kBound) {
        s.x = -kBound;
        s.vx = -s.vx;
    }
    if (s.y > kBound) {
        s.y = kBound;
        s.vy = -s.vy;
    } else if (s.y < -kBound) {
        s.y = -kBound;
        s.vy = -s.vy;
    }
    s.tick += 1;
}

uint64_t hashState(const State& s) {
    uint64_t h = kFnvOffset;
    h = fnvU32(h, floatBits(s.x));
    h = fnvU32(h, floatBits(s.y));
    h = fnvU32(h, floatBits(s.vx));
    h = fnvU32(h, floatBits(s.vy));
    h = fnvU32(h, static_cast<uint32_t>(s.tick & 0xffffffffu));
    h = fnvU32(h, static_cast<uint32_t>(s.tick >> 32));
    return h;
}

} // namespace rlios::sim
