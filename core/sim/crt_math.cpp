// Port of sinf/cosf from the Microsoft UCRT the game ships and loads (app-local ucrtbase.dll
// 10.0.26100.2454; the exe imports them through api-ms-win-crt-math-l1-1-0.dll).
//
// Evidence: ucrtbase.dll RVA 0xAC110 (sinf) and 0xA7D20 (cosf). Both test a CPU-feature flag
// (dword at RVA 0x138E60, live value 3 on the reference PC) and take an FMA3 code path when it is
// set, which is the case on every x64 CPU since 2013-2015. That path is the reference. It
// promotes the argument to double, evaluates a polynomial with FUSED multiply-adds and narrows
// the result back to float (cvtsd2ss).
//
// THIS IS THE ONLY SIM OBJECT ALLOWED TO CONTAIN FUSED MULTIPLY-ADDS (tools/check_no_fma.sh
// exempts it by name): the original fuses here, so we must too. Every fused operation is written
// as an explicit std::fma; the file is still compiled with -ffp-contract=off, so the compiler
// adds none of its own.
//
// Verified exhaustively: tools/harness/ucrt_check.cpp runs the real DLL under Wine and compares
// every float in [-pi/4, pi/4] bit for bit.
#include "sim/crt_math.h"

#include "sim/vec.h"

#include <atomic>
#include <cmath>

namespace rlios::sim {

namespace {

double bitsDouble(uint64_t b) {
    double d;
    std::memcpy(&d, &b, sizeof d);
    return d;
}

// |x| bit-pattern thresholds used by both routines.
constexpr uint32_t kTiny = 0x39000000u;      // 2^-13
constexpr uint32_t kSmall = 0x3C000000u;     // 2^-7
constexpr uint32_t kQuarterPi = 0x3F490FDBu; // pi/4 as float; above it the UCRT reduces the range

std::atomic<unsigned long long> gInexactCalls{0};

} // namespace

bool crtTrigInRange(float x) {
    return (floatBits(x) & 0x7FFFFFFFu) <= kQuarterPi;
}

float crtSinf(float x) {
    const uint32_t ax = floatBits(x) & 0x7FFFFFFFu;
    const double xd = static_cast<double>(x);
    if (ax > kQuarterPi) {
        // Range reduction path (RVA 0xAC1D0) is NOT ported. The only caller, integrateTransform,
        // clamps the angle so that |x| <= pi/8; callers must check crtTrigInRange() otherwise.
        gInexactCalls.fetch_add(1, std::memory_order_relaxed);
        return static_cast<float>(std::sin(xd));
    }
    if (ax < kTiny)
        return x;
    if (ax < kSmall) {
        // RVA 0xAC170: x - (x*x*x) * (1/6), fused.
        const double x3 = (xd * xd) * xd;
        return static_cast<float>(std::fma(-x3, bitsDouble(0x3FC5555555555555ull), xd));
    }
    // RVA 0xAC190.
    const double r = xd * xd;
    double p = std::fma(r, bitsDouble(0x3EC71DE3A556C734ull), bitsDouble(0xBF2A01A01A01A01Aull));
    p = std::fma(p, r, bitsDouble(0x3F81111111111111ull));
    p = std::fma(p, r, bitsDouble(0xBFC5555555555555ull));
    const double x3 = xd * r;
    return static_cast<float>(std::fma(p, x3, xd));
}

float crtCosf(float x) {
    const uint32_t ax = floatBits(x) & 0x7FFFFFFFu;
    const double xd = static_cast<double>(x);
    if (ax > kQuarterPi) {
        // Not ported (RVA 0xA7E00); see crtSinf.
        gInexactCalls.fetch_add(1, std::memory_order_relaxed);
        return static_cast<float>(std::cos(xd));
    }
    if (ax < kTiny)
        return 1.0f;
    if (ax < kSmall) {
        // RVA 0xA7D80: 1 - x * (x * 0.5), fused.
        return static_cast<float>(std::fma(-xd, xd * 0.5, 1.0));
    }
    // RVA 0xA7DA0.
    const double r = xd * xd;
    const double t = 1.0 - r * 0.5;
    double p = std::fma(r, bitsDouble(0xBE927E4FB7789F5Cull), bitsDouble(0x3EFA01A01A01A019ull));
    p = std::fma(p, r, bitsDouble(0xBF56C16C16C16C16ull));
    p = std::fma(p, r, bitsDouble(0x3FA5555555555555ull));
    return static_cast<float>(std::fma(p, r * r, t));
}

float crtAtan2f(float y, float x) {
    gInexactCalls.fetch_add(1, std::memory_order_relaxed);
    return static_cast<float>(std::atan2(static_cast<double>(y), static_cast<double>(x)));
}

unsigned long long crtInexactCalls() {
    return gInexactCalls.load(std::memory_order_relaxed);
}

} // namespace rlios::sim
