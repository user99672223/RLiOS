// crt_math must return the bits of the game's ucrtbase.dll on every platform (x86-64 uses libm's
// correctly rounded fma, arm64 the fmadd instruction). The full proof runs on the laptop against
// the real DLL (tools/harness/ucrt_check.sh: every float for 1-argument functions, structured
// sweeps plus 10^9 random pairs for 2-argument ones). This test guards the port in CI without the
// DLL: golden {input, output} pairs taken from the DLL, plus a dense hash over [-pi/4, pi/4].
#include "crt_golden.h"
#include "sim/crt_math.h"
#include "sim/vec.h"
#include "test.h"

using namespace rlios::sim;

namespace {

template <size_t N>
int check1(const char* name, float (*fn)(float), const crt_golden::Vec (&v)[N]) {
    int bad = 0;
    for (const crt_golden::Vec& g : v) {
        const uint32_t got = floatBits(fn(bitsFloat(g.a)));
        if (got != g.r) {
            if (bad < 5)
                std::fprintf(stderr, "%s(%08X): got %08X want %08X\n", name, g.a, got, g.r);
            ++bad;
        }
    }
    return bad;
}

template <size_t N>
int check2(const char* name, float (*fn)(float, float), const crt_golden::Vec (&v)[N]) {
    int bad = 0;
    for (const crt_golden::Vec& g : v) {
        const uint32_t got = floatBits(fn(bitsFloat(g.a), bitsFloat(g.b)));
        if (got != g.r) {
            if (bad < 5)
                std::fprintf(stderr, "%s(%08X, %08X): got %08X want %08X\n", name, g.a, g.b, got,
                             g.r);
            ++bad;
        }
    }
    return bad;
}

} // namespace

int main() {
    using namespace crt_golden;
    CHECK_EQ(check1("sinf", crtSinf, k_sinf), 0);
    CHECK_EQ(check1("cosf", crtCosf, k_cosf), 0);
    CHECK_EQ(check1("tanf", crtTanf, k_tanf), 0);
    CHECK_EQ(check1("asinf", crtAsinf, k_asinf), 0);
    CHECK_EQ(check1("acosf", crtAcosf, k_acosf), 0);
    CHECK_EQ(check1("atanf", crtAtanf, k_atanf), 0);
    CHECK_EQ(check1("expf", crtExpf, k_expf), 0);
    CHECK_EQ(check1("logf", crtLogf, k_logf), 0);
    CHECK_EQ(check1("floorf", crtFloorf, k_floorf), 0);
    CHECK_EQ(check1("ceilf", crtCeilf, k_ceilf), 0);
    CHECK_EQ(check1("sqrtf", crtSqrtf, k_sqrtf), 0);
    CHECK_EQ(check2("atan2f", crtAtan2f, k_atan2f), 0);
    CHECK_EQ(check2("powf", crtPowf, k_powf), 0);
    CHECK_EQ(check2("fmodf", crtFmodf, k_fmodf), 0);

    // Dense hash of sinf/cosf over [-pi/4, pi/4] (the ball integrator's range); unchanged since the
    // first port, so the full-range work cannot have moved these bits.
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
    CHECK(crtTrigInRange(bitsFloat(0x3F490FDBu)));
    CHECK(!crtTrigInRange(bitsFloat(0x3F490FDCu)));
    CHECK_EQ(crtInexactCalls(), 0u);
    TEST_MAIN_RETURN();
}
