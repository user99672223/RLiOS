// crt_math must return the same bits on every platform (x86-64 uses libm's software/hardware
// fma, arm64 the fmadd instruction). The expected hash was produced by the port after it had
// been compared against the real ucrtbase.dll for every float in [-pi/4, pi/4]
// (tools/harness/ucrt_check.cpp, 2,123,505,592 inputs, 0 mismatches).
#include "sim/crt_math.h"
#include "sim/vec.h"
#include "test.h"

using namespace rlios::sim;

int main() {
    uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&h](uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= (v >> (8 * i)) & 0xffu;
            h *= 0x100000001b3ull;
        }
    };
    for (uint32_t sign = 0; sign < 2; ++sign) {
        for (uint32_t b = 0; b <= 0x3F490FDBu; b += 257) {
            const float x = bitsFloat(b | (sign << 31));
            mix(floatBits(crtSinf(x)));
            mix(floatBits(crtCosf(x)));
        }
    }
    std::printf("crt hash %016llx\n", static_cast<unsigned long long>(h));
    CHECK_EQ(h, 0x065a7f54ff1d286dull);
    // Spot values (bits returned by the Microsoft routine).
    CHECK_EQ(floatBits(crtCosf(0.0f)), 0x3F800000u);
    CHECK_EQ(floatBits(crtSinf(bitsFloat(0x38000000u))), 0x38000000u);
    CHECK(crtTrigInRange(bitsFloat(0x3F490FDBu)));
    CHECK(!crtTrigInRange(bitsFloat(0x3F490FDCu)));
    TEST_MAIN_RETURN();
}
