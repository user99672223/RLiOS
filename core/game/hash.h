// FNV-1a 64 over little-endian bytes of integers and float BIT PATTERNS (never epsilons).
#pragma once

#include <cstdint>

#include "sim/vec.h"

namespace rlios::game {

struct Fnv {
    uint64_t v = 0xcbf29ce484222325ull;
    void u32(uint32_t x) {
        for (int i = 0; i < 4; ++i) {
            v ^= static_cast<uint64_t>((x >> (8 * i)) & 0xffu);
            v *= 0x100000001b3ull;
        }
    }
    void u64(uint64_t x) {
        u32(static_cast<uint32_t>(x));
        u32(static_cast<uint32_t>(x >> 32));
    }
    void f(float x) { u32(sim::floatBits(x)); }
    void vec(const sim::Vec3& a) {
        f(a.x);
        f(a.y);
        f(a.z);
    }
};

} // namespace rlios::game
