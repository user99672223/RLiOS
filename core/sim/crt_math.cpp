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

#include <climits>
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

uint64_t doubleBits(double d) {
    uint64_t b;
    std::memcpy(&b, &d, sizeof b);
    return b;
}

// x86 cvttpd2dq: truncation, out of range or NaN gives the "integer indefinite" 0x80000000.
int32_t cvttToInt32(double d) {
    if (!(d > -2147483649.0 && d < 2147483648.0))
        return INT32_MIN;
    return static_cast<int32_t>(d);
}

// NaN and infinity handler shared by sinf/cosf (ucrtbase RVA 0xE26F0, reached through 0xE2780 /
// 0xE2610): a NaN comes back quieted (bit 22 set), an infinity gives the default NaN 0xFFC00000.
float trigNanInf(uint32_t bits) {
    if (bits & 0x007FFFFFu)
        return bitsFloat(bits | 0x00400000u);
    return bitsFloat(0xFFC00000u);
}

// Bits of 2/pi the large-argument reductions read, ucrtbase RVA 0x106370 + 0x70 .. +0xA0. The
// code loads unaligned little-endian qwords at byte offset 0x86 - (exponent >> 3) (0x77..0x83)
// and the two following qwords.
constexpr uint32_t kTwoOverPiBase = 0x70;
constexpr uint8_t kTwoOverPi[48] = {
    0x37, 0x09, 0xE9, 0xB8, 0x91, 0xDC, 0x86, 0x15, 0xEF, 0x7A, 0xAF, 0x8E, 0x45, 0xF9, 0x07, 0x41,
    0x0E, 0xF1, 0x64, 0x56, 0x8A, 0x6D, 0x03, 0x77, 0xD3, 0xD4, 0x47, 0x5F, 0x9D, 0xF0, 0xA7, 0x54,
    0x10, 0x39, 0xB9, 0x0D, 0xE6, 0x8B, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

uint64_t twoOverPiQword(uint64_t offset) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | kTwoOverPi[offset - kTwoOverPiBase + static_cast<uint64_t>(i)];
    return v;
}

// x86 bsr: index of the highest set bit; with a zero source the destination keeps its old value
// (AMD documents it, Intel CPUs behave the same).
uint64_t bsr64(uint64_t v, uint64_t old) {
    return v ? static_cast<uint64_t>(63 - __builtin_clzll(v)) : old;
}

// Payne-Hanek reduction of |x| (as double bits) by pi/2, shared code of ucrtbase RVA 0xAC240
// (sinf, inline) and 0xAB4C0 (cosf helper) up to the point where the high fraction is formed.
// Integer register semantics are reproduced exactly (64-bit wraparound, shift counts mod 64).
struct LargeReduction {
    uint64_t hi;   // r10: double bits of the reduced fraction (in units of pi/2), signed
    uint64_t lo;   // r9: the next 64 bits of the fraction after normalization
    uint64_t exp;  // r11: biased exponent of hi
    uint64_t sign; // rdx: 0 or 1<<63
    uint32_t quad; // quadrant, 0..3
};

LargeReduction payneHanek(uint64_t absBits) {
    using u128 = unsigned __int128;
    const uint64_t e = (absBits >> 52) - 0x3FF;
    const uint64_t off = 0x86 - (e >> 3);
    const uint64_t m = (absBits & 0x000FFFFFFFFFFFFFull) | (1ull << 52);
    const u128 p0 = static_cast<u128>(twoOverPiQword(off)) * m;
    uint64_t r8 = static_cast<uint64_t>(p0);
    const u128 p1 = static_cast<u128>(twoOverPiQword(off + 8)) * m;
    const u128 s1 = p1 + static_cast<uint64_t>(p0 >> 64);
    uint64_t r9 = static_cast<uint64_t>(s1);
    uint64_t r10 = static_cast<uint64_t>(s1 >> 64);
    r10 += static_cast<uint64_t>(static_cast<u128>(twoOverPiQword(off + 16)) * m);
    const uint64_t shift = 0x36 - (e & 7);
    uint64_t q = r10 >> shift;
    const uint64_t carry = (r10 >> (shift - 1)) & 1;
    uint64_t sign = 0;
    if (carry) {
        r10 = ~r10;
        r9 = ~r9;
        r8 = ~r8;
        sign = 0x8000000000000000ull;
    }
    q += carry;
    uint64_t c = (e & 7) + 10;
    r10 = (r10 << c) >> c;
    c -= 0x40;
    uint64_t r11 = c;
    if (r10 != 0) {
        c = bsr64(r10, c);
    } else {
        r10 = r9;
        r9 = r8;
        r8 = 0;
        c = bsr64(r10, c);
        r11 -= 0x40;
    }
    r11 += c;
    c -= 0x34;
    if (static_cast<int64_t>(c) < 0) {
        c = 0 - c;
        const uint64_t rax = r9;
        r10 <<= (c & 63);
        r9 <<= (c & 63);
        c = 0x40 - c;
        r10 |= rax >> (c & 63);
        r8 >>= (c & 63);
        r9 |= r8;
    } else if (c != 0) {
        r8 = r10;
        r10 >>= (c & 63);
        r9 >>= (c & 63);
        c = 0x40 - c;
        r8 <<= (c & 63);
        r9 |= r8;
    }
    r11 += 0x3FF;
    r10 &= ~(1ull << 52);
    r10 |= sign;
    r10 |= r11 << 52;
    return {r10, r9, r11, sign, static_cast<uint32_t>(q & 3)};
}

// Polynomials of the FMA3 path (constants at ucrtbase RVA 0x109E70..0x109EB8).
double sinPoly(double r) { // RVA 0xAC3A4 (sinf) / 0xA7E9C (cosf)
    const double r2 = r * r;
    double p = std::fma(r2, bitsDouble(0x3EC71DE3A556C734ull), bitsDouble(0xBF2A01A01A01A01Aull));
    p = std::fma(p, r2, bitsDouble(0x3F81111111111111ull));
    p = std::fma(p, r2, bitsDouble(0xBFC5555555555555ull));
    const double r3 = r * r2;
    return std::fma(p, r3, r);
}

double cosPolySinf(double r) { // RVA 0xAC3E0: here 1 - r^2/2 is fused
    const double r2 = r * r;
    const double t = std::fma(r2, bitsDouble(0xBFE0000000000000ull), 1.0);
    double p = std::fma(r2, bitsDouble(0xBE927E4FB7789F5Cull), bitsDouble(0x3EFA01A01A01A019ull));
    p = std::fma(p, r2, bitsDouble(0xBF56C16C16C16C16ull));
    p = std::fma(p, r2, bitsDouble(0x3FA5555555555555ull));
    return std::fma(p, r2 * r2, t);
}

double cosPolyCosf(double r) { // RVA 0xA7DA0 / 0xA7ED0: 1 - r^2*0.5 rounded separately
    const double r2 = r * r;
    const double t = 1.0 - r2 * 0.5;
    double p = std::fma(r2, bitsDouble(0xBE927E4FB7789F5Cull), bitsDouble(0x3EFA01A01A01A019ull));
    p = std::fma(p, r2, bitsDouble(0xBF56C16C16C16C16ull));
    p = std::fma(p, r2, bitsDouble(0x3FA5555555555555ull));
    return std::fma(p, r2 * r2, t);
}

// Medium-range Cody-Waite reduction (sinf RVA 0xAC1DC, cosf RVA 0xA7E0C): n = trunc(|x|*2/pi + 0.5)
// (fused), r = (|x| - n*pio2_1 (fused)) - n*pio2_1t.
double mediumReduce(double ax, uint32_t& quad) {
    const double t = std::fma(ax, bitsDouble(0x3FE45F306DC9C883ull), 0.5);
    const int32_t n = cvttToInt32(t);
    quad = static_cast<uint32_t>(n) & 3u;
    const double nd = static_cast<double>(n);
    const double r1 = std::fma(-nd, bitsDouble(0x3FF921FB54400000ull), ax);
    return r1 - nd * bitsDouble(0x3DD0B4611A626331ull);
}

} // namespace

bool crtTrigInRange(float x) {
    return (floatBits(x) & 0x7FFFFFFFu) <= kQuarterPi;
}

float crtSinf(float x) {
    const uint32_t ax = floatBits(x) & 0x7FFFFFFFu;
    const double xd = static_cast<double>(x);
    if (ax >= 0x7F800000u)
        return trigNanInf(floatBits(x)); // RVA 0xAC458
    if (ax > kQuarterPi) {
        // RVA 0xAC1D0: reduce |x| by pi/2, evaluate sin or cos by quadrant, then fix the sign.
        const double axd = bitsDouble(doubleBits(xd) & 0x7FFFFFFFFFFFFFFFull);
        uint32_t quad;
        double r;
        if (ax < 0x4B800456u) {
            r = mediumReduce(axd, quad);
        } else {
            // RVA 0xAC240: only the high fraction is used, times pi/2 in one rounding.
            const LargeReduction red = payneHanek(doubleBits(axd));
            quad = red.quad;
            r = bitsDouble(red.hi) * bitsDouble(0x3FF921FB54442D18ull);
        }
        double y = (quad & 1) ? cosPolySinf(r) : sinPoly(r);
        // RVA 0xAC425: negate unless quadrant is 2 or 3, then negate again for x >= 0.
        uint64_t yb = doubleBits(y);
        if (quad < 2)
            yb ^= 0x8000000000000000ull;
        if (!(doubleBits(xd) >> 63))
            yb ^= 0x8000000000000000ull;
        y = bitsDouble(yb);
        return static_cast<float>(y);
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
    if (ax >= 0x7F800000u)
        return trigNanInf(floatBits(x)); // RVA 0xA7F35
    if (ax > kQuarterPi) {
        // RVA 0xA7E00.
        const double axd = bitsDouble(doubleBits(xd) & 0x7FFFFFFFFFFFFFFFull);
        uint32_t quad;
        double r;
        if (ax < 0x4F490FDBu) {
            r = mediumReduce(axd, quad);
        } else {
            // RVA 0xAB4C0: the fraction as hi + lo doubles, times pi/2 in split precision.
            const LargeReduction red = payneHanek(doubleBits(axd));
            quad = red.quad;
            const uint64_t idx = bsr64(red.lo, red.exp << 52);
            uint64_t c = 0x40 - idx;
            uint64_t lo = (red.lo << (c & 63)) >> 12;
            c += 0x34;
            lo |= red.sign | ((red.exp - c) << 52);
            const double f = bitsDouble(red.hi);
            const double fl = bitsDouble(lo);
            const double fh = bitsDouble(red.hi & 0xFFFFFFFFF8000000ull);
            const double ft = f - fh;
            const double pio2 = bitsDouble(0x3FF921FB54442D18ull);
            const double pio2Hi = bitsDouble(0x3FF921FB50000000ull);
            const double pio2Lo = bitsDouble(0x3E5110B460000000ull);
            const double p = f * pio2;
            double e = fh * pio2Hi - p;
            e = std::fma(ft, pio2Hi, e);
            e = std::fma(fh, pio2Lo, e);
            e = std::fma(ft, pio2Lo, e);
            const double t = std::fma(f, bitsDouble(0x3C91A62633145C06ull), fl * pio2);
            r = p + (e + t);
        }
        double y = (quad & 1) ? sinPoly(r) : cosPolyCosf(r);
        // RVA 0xA7F18: sign bit = ((quad + 1) >> 1) & 1.
        if (((quad + 1) >> 1) & 1)
            y = bitsDouble(doubleBits(y) ^ 0x8000000000000000ull);
        return static_cast<float>(y);
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

// atanf: ucrtbase RVA 0x79430, FMA3 path 0x7944C. Double precision, argument reduction to
// |t| < 0.4375 around atan(0), atan(1/2), atan(1), atan(3/2), atan(inf); rational kernel.
float crtAtanf(float x) {
    const double xd = static_cast<double>(x);
    const bool neg = (doubleBits(xd) >> 63) != 0;
    const uint64_t ab = doubleBits(xd) & 0x7FFFFFFFFFFFFFFFull;
    double a = bitsDouble(ab);
    double base = 0.0;
    if (ab >= 0x3FDC000000000000ull) {
        if (ab < 0x3FE6000000000000ull) { // RVA 0x7957B
            base = bitsDouble(0x3FDDAC670561BB4Full);
            a = (a + a - 1.0) / (a + 2.0);
        } else if (ab < 0x3FF3000000000000ull) { // RVA 0x79515
            base = bitsDouble(0x3FE921FB54442D18ull);
            a = (a - 1.0) / (a + 1.0);
        } else if (ab < 0x4003800000000000ull) { // RVA 0x795A0
            base = bitsDouble(0x3FEF730BD281F69Bull);
            a = (a - 1.5) / std::fma(a, 1.5, 1.0);
        } else if (ab > 0x7FF0000000000000ull) { // RVA 0x795E6: NaN, quieted
            return bitsFloat(floatBits(x) | 0x00400000u);
        } else if (a > bitsDouble(0x43D3200000000000ull)) { // RVA 0x795CA
            return bitsFloat(neg ? 0xBFC90FDBu : 0x3FC90FDBu);
        } else { // RVA 0x79562
            base = bitsDouble(0x3FF921FB54442D18ull);
            a = -1.0 / a;
        }
    }
    // RVA 0x79489.
    const double t2 = a * a;
    double p = std::fma(t2, bitsDouble(0x3F73476A758DA22Aull), bitsDouble(0x3FC89E173A81EE7Full));
    p = std::fma(t2, p, bitsDouble(0x3FD2FA5316907834ull));
    const double num = p * (a * t2);
    double q = std::fma(t2, bitsDouble(0x3FD327E3DF2CF2AAull), bitsDouble(0x3FF1C58793DA6EA4ull));
    q = std::fma(t2, q, bitsDouble(0x3FEC777CA21053F0ull));
    double r = base - (num / q - a);
    if (neg)
        r = bitsDouble(doubleBits(r) ^ 0x8000000000000000ull);
    return static_cast<float>(r);
}

namespace {

// Rational kernel shared by asinf (RVA 0x2F7BA) and acosf (RVA 0x725FF), single precision
// with fused steps; returns r with asin(v) ~ v + v*r, z = v*v. Constants at RVA 0x103FD0..0x103FE4.
float asinKernel(float z) {
    float p = std::fma(-z, bitsFloat(0x3B81CE6Bu), bitsFloat(0xBC5B3FE1u));
    p = std::fma(z, p, -bitsFloat(0x3D678BDDu));
    p = std::fma(z, p, bitsFloat(0x3E3C94DCu));
    const float num = p * z;
    const float den = std::fma(-z, bitsFloat(0x3F561F0Du), bitsFloat(0x3F8D6FA5u));
    return num / den;
}

} // namespace

// asinf: ucrtbase RVA 0x2F710, FMA3 path 0x2F72C (single precision).
float crtAsinf(float x) {
    const uint32_t bits = floatBits(x);
    const uint32_t ab = bits & 0x7FFFFFFFu;
    const uint32_t ex = (bits >> 23) & 0xFFu;
    if (ab > 0x7F800000u)
        return bitsFloat(bits | 0x00400000u); // RVA 0x2F8B6
    if (ex < 0x71)
        return x;     // RVA 0x2F8A7: |x| < 2^-14
    if (ex >= 0x7F) { // RVA 0x2F8C3
        if (x == 1.0f)
            return bitsFloat(0x3FC90FDBu);
        if (x == -1.0f)
            return bitsFloat(0xBFC90FDBu);
        return bitsFloat(0xFFC00000u); // domain error handler (RVA 0x32340) default result
    }
    const float a = bitsFloat(ab);
    float res;
    if (ex < 0x7E) { // RVA 0x2F89C
        const float r = asinKernel(a * a);
        res = std::fma(r, a, a);
    } else { // |x| >= 0.5: asin = pi/2 - 2 asin(sqrt((1-a)/2)), RVA 0x2F795 / 0x2F840
        const float z = (1.0f - a) * 0.5f;
        const float sq = std::sqrt(z);
        const float r = asinKernel(z);
        const float df = bitsFloat(floatBits(sq) & 0xFFFF0000u);
        const float c = std::fma(-df, df, z) / (df + sq);
        float t = bitsFloat(0x33A22168u) - (c + c);
        t = std::fma(r, sq + sq, -t);
        const float u = bitsFloat(0x3F490FDAu) - (df + df);
        res = bitsFloat(0x3F490FDAu) - (t - u);
    }
    return (bits >> 31) ? bitsFloat(floatBits(res) ^ 0x80000000u) : res;
}

// acosf: ucrtbase RVA 0x72560, FMA3 path 0x7257C.
float crtAcosf(float x) {
    const uint32_t bits = floatBits(x);
    const uint32_t ab = bits & 0x7FFFFFFFu;
    const uint32_t ex = (bits >> 23) & 0xFFu;
    if (ab > 0x7F800000u)
        return bitsFloat(bits | 0x00400000u); // RVA 0x72722
    if (ex < 0x65)
        return bitsFloat(0x3FC90FDBu); // RVA 0x7272F: |x| < 2^-26
    if (ex >= 0x7F) {                  // RVA 0x726CC
        if (x == 1.0f)
            return 0.0f;
        if (x == -1.0f)
            return bitsFloat(0x40490FDBu);
        return bitsFloat(0xFFC00000u);
    }
    const float a = bitsFloat(ab);
    if (ex < 0x7E) { // RVA 0x72784 then 0x72691: pi/2 - (x - (pio2_lo - x*r)) in double
        const float r = asinKernel(a * a);
        const double xr = static_cast<double>(r * x);
        const double t = bitsDouble(0x3C91A62633145C07ull) - xr;
        return static_cast<float>(bitsDouble(0x3FF921FB54442D18ull) - (static_cast<double>(x) - t));
    }
    const float z = (1.0f - a) * 0.5f;
    const float sq = std::sqrt(z);
    const float r = asinKernel(z);
    if (bits >> 31) { // RVA 0x726F8: pi - 2*(s + (s*r - pio2_lo)) in double
        const double sr = static_cast<double>(sq * r);
        const double t = (sr - bitsDouble(0x3C91A62633145C07ull)) + static_cast<double>(sq);
        return static_cast<float>(bitsDouble(0x400921FB54442D18ull) - (t + t));
    }
    // RVA 0x72647: 2*(df + c + s*r) with s = df + c split.
    const float df = bitsFloat(floatBits(sq) & 0xFFFF0000u);
    const float c = std::fma(-df, df, z) / (df + sq);
    const float t = std::fma(r, sq + sq, c + c);
    return t + (df + df);
}

namespace {
// tanf kernel, ucrtbase RVA 0xAD55E / 0xAD761: r + r^3 * P(r^2)/Q(r^2), constants RVA
// 0x10B0F0..0x10B110.
double tanKernel(double r) {
    const double r2 = r * r;
    const double num =
        std::fma(r2, bitsDouble(0xBF919DBA6EFD6AADull), bitsDouble(0x3FD8A8B0DA56CB17ull));
    double den = std::fma(r2, bitsDouble(0x3F92E29003C692D9ull), bitsDouble(0xBFE07266D7B3511Bull));
    den = std::fma(r2, den, bitsDouble(0x3FF27E84A3E73A2Eull));
    return std::fma(r2 * r, num / den, r);
}
} // namespace

// tanf: ucrtbase RVA 0xAD4F0, FMA3 path (same reductions as sinf/cosf).
float crtTanf(float x) {
    const uint32_t ax = floatBits(x) & 0x7FFFFFFFu;
    if (ax >= 0x7F800000u)
        return trigNanInf(floatBits(x)); // RVA 0xAD7C5
    const double xd = static_cast<double>(x);
    if (ax <= kQuarterPi) {
        if (ax < 0x32000000u)
            return x;   // RVA 0xAD533
        if (ax < kTiny) // RVA 0xAD544: x + x^3/3, fused
            return static_cast<float>(
                std::fma((xd * xd) * xd, bitsDouble(0x3FD5555555555555ull), xd));
        return static_cast<float>(tanKernel(xd)); // RVA 0xAD55A
    }
    const uint64_t absBits = doubleBits(xd) & 0x7FFFFFFFFFFFFFFFull;
    uint32_t quad;
    double r;
    if (absBits < 0x41E921FB40000000ull) { // RVA 0xAD5A4: |x| < 2^31 * pi/2
        r = mediumReduce(bitsDouble(absBits), quad);
    } else { // RVA 0xAD606: as sinf
        const LargeReduction red = payneHanek(absBits);
        quad = red.quad;
        r = bitsDouble(red.hi) * bitsDouble(0x3FF921FB54442D18ull);
    }
    double y = tanKernel(r);
    if (quad & 1)
        y = -1.0 / y; // RVA 0xAD7A2
    y = bitsDouble(doubleBits(y) ^ (doubleBits(xd) & 0x8000000000000000ull));
    return static_cast<float>(y);
}

// atan2f: ucrtbase RVA 0x33DA0, FMA3 path at 0x33DC0 (taken when the feature dword at 0x138E60
// has both low bits set). Evaluated in double from the float arguments.
namespace {

// atan(k/256) for k = 16..256, ucrtbase RVA 0x100F50 (241 doubles).
constexpr uint64_t kAtanTable[241] = {
    0x3FAFF55BB72CFDE9ull, 0x3FB0F99EA71D52A6ull, 0x3FB1F86DBF082D58ull, 0x3FB2F719318A4A9Aull,
    0x3FB3F59F0E7C559Dull, 0x3FB4F3FD677292FBull, 0x3FB5F2324FD2D7B2ull, 0x3FB6F03BDCEA4B0Cull,
    0x3FB7EE182602F10Eull, 0x3FB8EBC54478FB28ull, 0x3FB9E94153CFDCF1ull, 0x3FBAE68A71C722B8ull,
    0x3FBBE39EBE6F07C3ull, 0x3FBCE07C5C3CCA32ull, 0x3FBDDD21701EBA6Eull, 0x3FBED98C2190043Aull,
    0x3FBFD5BA9AAC2F6Dull, 0x3FC068D584212B3Dull, 0x3FC0E6ADCCF40881ull, 0x3FC1646541060850ull,
    0x3FC1E1FAFB043726ull, 0x3FC25F6E171A535Cull, 0x3FC2DCBDB2FBA1FFull, 0x3FC359E8EDEB99A3ull,
    0x3FC3D6EEE8C6626Cull, 0x3FC453CEC6092A9Eull, 0x3FC4D087A9DA4F17ull, 0x3FC54D18BA11570Aull,
    0x3FC5C9811E3EC269ull, 0x3FC645BFFFB3AA73ull, 0x3FC6C1D4898933D8ull, 0x3FC73DBDE8A7D201ull,
    0x3FC7B97B4BCE5B02ull, 0x3FC8350BE398EBC7ull, 0x3FC8B06EE2879C28ull, 0x3FC92BA37D050271ull,
    0x3FC9A6A8E96C8626ull, 0x3FCA217E601081A5ull, 0x3FCA9C231B403279ull, 0x3FCB1696574D780Bull,
    0x3FCB90D7529260A2ull, 0x3FCC0AE54D768466ull, 0x3FCC84BF8A742E6Dull, 0x3FCCFE654E1D5395ull,
    0x3FCD77D5DF205736ull, 0x3FCDF110864C9D9Dull, 0x3FCE6A148E96EC4Dull, 0x3FCEE2E1451D980Cull,
    0x3FCF5B75F92C80DDull, 0x3FCFD3D1FC40DBE4ull, 0x3FD025FA510665B5ull, 0x3FD061EEA03D6290ull,
    0x3FD09DC597D86362ull, 0x3FD0D97EE509ACB3ull, 0x3FD1151A362431C9ull, 0x3FD150973A9CE546ull,
    0x3FD18BF5A30BF178ull, 0x3FD1C735212DD883ull, 0x3FD2025567E47C95ull, 0x3FD23D562B381041ull,
    0x3FD278372057EF45ull, 0x3FD2B2F7FD9B5FE2ull, 0x3FD2ED987A823CFEull, 0x3FD328184FB58951ull,
    0x3FD362773707EBCBull, 0x3FD39CB4EB76157Bull, 0x3FD3D6D129271134ull, 0x3FD410CBAD6C7D32ull,
    0x3FD44AA436C2AF09ull, 0x3FD4845A84D0C21Bull, 0x3FD4BDEE586890E6ull, 0x3FD4F75F73869978ull,
    0x3FD530AD9951CD49ull, 0x3FD569D88E1B4CD7ull, 0x3FD5A2E0175E0F4Eull, 0x3FD5DBC3FBBE768Dull,
    0x3FD614840309CFE1ull, 0x3FD64D1FF635C1C5ull, 0x3FD685979F5FA6FDull, 0x3FD6BDEAC9CBD76Cull,
    0x3FD6F61941E4DEF0ull, 0x3FD72E22D53AA2A9ull, 0x3FD7660752817501ull, 0x3FD79DC6899118D1ull,
    0x3FD7D5604B63B3F7ull, 0x3FD80CD46A14B1D0ull, 0x3FD84422B8DF95D7ull, 0x3FD87B4B0C1EBEDBull,
    0x3FD8B24D394A1B25ull, 0x3FD8E92916F5CDE8ull, 0x3FD91FDE7CD0C662ull, 0x3FD9566D43A34907ull,
    0x3FD98CD5454D6B18ull, 0x3FD9C3165CC58107ull, 0x3FD9F93066168001ull, 0x3FDA2F233E5E530Bull,
    0x3FDA64EEC3CC23FCull, 0x3FDA9A92D59E98CFull, 0x3FDAD00F5422058Bull, 0x3FDB056420AE9343ull,
    0x3FDB3A911DA65C6Cull, 0x3FDB6F962E737EFBull, 0x3FDBA473378624A5ull, 0x3FDBD9281E528191ull,
    0x3FDC0DB4C94EC9EFull, 0x3FDC42191FF11EB6ull, 0x3FDC76550AAD71F8ull, 0x3FDCAA6872F3631Bull,
    0x3FDCDE53432C1350ull, 0x3FDD121566B7F2ADull, 0x3FDD45AEC9EC862Bull, 0x3FDD791F5A1226F4ull,
    0x3FDDAC670561BB4Full, 0x3FDDDF85BB026974ull, 0x3FDE127B6B0744AFull, 0x3FDE4548066CF51Aull,
    0x3FDE77EB7F175A34ull, 0x3FDEAA65C7CF28C4ull, 0x3FDEDCB6D43F8434ull, 0x3FDF0EDE98F393CFull,
    0x3FDF40DD0B541417ull, 0x3FDF72B221A4E495ull, 0x3FDFA45DD3029258ull, 0x3FDFD5E0175FDF83ull,
    0x3FE0039C73C1A40Bull, 0x3FE01C341E82422Dull, 0x3FE034B709250488ull, 0x3FE04D25314342E5ull,
    0x3FE0657E94DB30CFull, 0x3FE07DC3324E9B38ull, 0x3FE095F30861A58Full, 0x3FE0AE0E1639866Cull,
    0x3FE0C6145B5B43DAull, 0x3FE0DE05D7AA6F7Cull, 0x3FE0F5E28B67E295ull, 0x3FE10DAA77307A0Dull,
    0x3FE1255D9BFBD2A8ull, 0x3FE13CFBFB1B056Eull, 0x3FE1548596376469ull, 0x3FE16BFA6F5137E1ull,
    0x3FE1835A88BE7C13ull, 0x3FE19AA5E5299F99ull, 0x3FE1B1DC87904284ull, 0x3FE1C8FE7341F64Full,
    0x3FE1E00BABDEFEB3ull, 0x3FE1F7043557138Aull, 0x3FE20DE813E823B1ull, 0x3FE224B74C1D192Aull,
    0x3FE23B71E2CC9E6Aull, 0x3FE25217DD17E501ull, 0x3FE268A940696DA6ull, 0x3FE27F261273D1B3ull,
    0x3FE2958E59308E30ull, 0x3FE2ABE21ADED073ull, 0x3FE2C2215E024465ull, 0x3FE2D84C2961E48Bull,
    0x3FE2EE628406CBCAull, 0x3FE30464753B090Aull, 0x3FE31A52048874BEull, 0x3FE3302B39B78856ull,
    0x3FE345F01CCE37BBull, 0x3FE35BA0B60ECCCEull, 0x3FE3713D0DF6C503ull, 0x3FE386C52D3DB11Eull,
    0x3FE39C391CD41719ull, 0x3FE3B198E5E2564Aull, 0x3FE3C6E491C78DC4ull, 0x3FE3DC1C2A188504ull,
    0x3FE3F13FB89E96F4ull, 0x3FE4064F47569F48ull, 0x3FE41B4AE06FEA41ull, 0x3FE430328E4B26D5ull,
    0x3FE445065B795B55ull, 0x3FE459C652BADC7Full, 0x3FE46E727EFE4715ull, 0x3FE4830AEB5F7BFDull,
    0x3FE4978FA3269EE1ull, 0x3FE4AC00B1C71762ull, 0x3FE4C05E22DE94E4ull, 0x3FE4D4A8023414E8ull,
    0x3FE4E8DE5BB6EC04ull, 0x3FE4FD013B7DD17Eull, 0x3FE51110ADC5ED81ull, 0x3FE5250CBEF1E9FAull,
    0x3FE538F57B89061Eull, 0x3FE54CCAF0362C8Full, 0x3FE5608D29C70C34ull, 0x3FE5743C352B33B9ull,
    0x3FE587D81F732FBAull, 0x3FE59B60F5CFAB9Dull, 0x3FE5AED6C5909517ull, 0x3FE5C2399C244260ull,
    0x3FE5D58987169B18ull, 0x3FE5E8C6941043CFull, 0x3FE5FBF0D0D5CC49ull, 0x3FE60F084B46E05Eull,
    0x3FE6220D115D7B8Dull, 0x3FE634FF312D1F3Bull, 0x3FE647DEB8E20B8Full, 0x3FE65AABB6C07B02ull,
    0x3FE66D663923E086ull, 0x3FE6800E4E7E2857ull, 0x3FE692A40556FB6Aull, 0x3FE6A5276C4B0575ull,
    0x3FE6B798920B3D98ull, 0x3FE6C9F7855C3198ull, 0x3FE6DC44551553AEull, 0x3FE6EE7F10204AEFull,
    0x3FE700A7C5784633ull, 0x3FE712BE84295198ull, 0x3FE724C35B4FAE7Bull, 0x3FE736B65A172DFFull,
    0x3FE748978FBA8E0Full, 0x3FE75A670B82D8D8ull, 0x3FE76C24DCC6C6C0ull, 0x3FE77DD112EA22C7ull,
    0x3FE78F6BBD5D315Eull, 0x3FE7A0F4EB9C19A2ull, 0x3FE7B26CAD2E50FDull, 0x3FE7C3D311A6092Bull,
    0x3FE7D528289FA093ull, 0x3FE7E66C01C114FDull, 0x3FE7F79EACB97898ull, 0x3FE808C03940694Aull,
    0x3FE819D0B7158A4Cull, 0x3FE82AD036000005ull, 0x3FE83BBEC5CDEE22ull, 0x3FE84C9C7653F7EAull,
    0x3FE85D69576CC2C5ull, 0x3FE86E2578F87AE5ull, 0x3FE87ED0EADC5A2Aull, 0x3FE88F6BBD023118ull,
    0x3FE89FF5FF57F1F7ull, 0x3FE8B06FC1CF3DFEull, 0x3FE8C0D9145CF49Dull, 0x3FE8D13206F8C4CAull,
    0x3FE8E17AA99CC05Dull, 0x3FE8F1B30C44F167ull, 0x3FE901DB3EEEF187ull, 0x3FE911F35199833Bull,
    0x3FE921FB54442D18ull,
};

// RVA 0x344F8: v * 2^-100 by bit manipulation, rounding half up on the magnitude.
uint64_t scaleDown100(uint64_t v) {
    const uint64_t sign = v & 0x8000000000000000ull;
    uint64_t mag = v & 0x7FFFFFFFFFFFFFFFull;
    const uint32_t e = static_cast<uint32_t>(mag >> 52);
    if (e > 100)
        return sign | ((static_cast<uint64_t>(e) - 100) << 52) | (mag & 0x800FFFFFFFFFFFFFull);
    mag = (mag & 0x801FFFFFFFFFFFFFull) | 0x0010000000000000ull;
    if (static_cast<int32_t>(101 - e) > 0x36)
        return sign;
    const uint64_t t = mag >> ((100 - static_cast<uint64_t>(e)) & 63);
    return sign | ((t >> 1) + (t & 1));
}

} // namespace

float crtAtan2f(float y, float x) {
    constexpr uint64_t kInf = 0x7FF0000000000000ull;
    const double xd = static_cast<double>(x);
    const double yd = static_cast<double>(y);
    const uint64_t xb = doubleBits(xd), yb = doubleBits(yd);
    const uint64_t ax = xb & 0x7FFFFFFFFFFFFFFFull, ay = yb & 0x7FFFFFFFFFFFFFFFull;
    const bool xNeg = (xb >> 63) != 0, yNeg = (yb >> 63) != 0;
    const int32_t de =
        static_cast<int32_t>((yb >> 52) & 0x7FF) - static_cast<int32_t>((xb >> 52) & 0x7FF);
    if (ax > kInf)
        return bitsFloat(floatBits(x) | 0x00400000u); // RVA 0x33F86: quiet x
    if (ay > kInf)
        return bitsFloat(floatBits(y) | 0x00400000u); // RVA 0x340D7: quiet y
    if (ay == 0)                                      // RVA 0x33F9C
        return xNeg ? bitsFloat(yNeg ? 0xC0490FDBu : 0x40490FDBu) : y;
    if (ax == 0 && yNeg) // RVA 0x34017 (x == 0, y > 0 continues below and ends at +pi/2)
        return bitsFloat(0xBFC90FDBu);
    if (de > 26) // RVA 0x34037
        return bitsFloat(yNeg ? 0xBFC90FDBu : 0x3FC90FDBu);
    if (de < -13 && !xNeg) { // RVA 0x33FF0: |y/x| tiny, x > 0
        if (de < -150)
            return bitsFloat(yNeg ? 0x80000000u : 0u);
        if (de < -126) { // RVA 0x34053: scale up, divide, scale back down by hand
            const double q = (bitsDouble(0x4630000000000000ull) * yd) / xd;
            return static_cast<float>(bitsDouble(scaleDown100(doubleBits(q))));
        }
        return static_cast<float>(yd / xd);
    }
    if (de < -26 && xNeg) // RVA 0x340EC
        return bitsFloat(yNeg ? 0xC0490FDBu : 0x40490FDBu);
    if (ay == kInf && ax == kInf) { // RVA 0x34111
        if (!xNeg)
            return bitsFloat(yNeg ? 0xBF490FDBu : 0x3F490FDBu);
        return bitsFloat(yNeg ? 0xC016CBE4u : 0x4016CBE4u);
    }
    // RVA 0x33E92: r = min/max of |x|, |y|.
    double big = bitsDouble(ax), small = bitsDouble(ay);
    bool swapped = false;
    if (small > big) {
        const double t = small;
        small = big;
        big = t;
        swapped = true;
    }
    double r = small / big;
    if (r > 0.0625) { // RVA 0x33ED9: atan(k/256) + atan(u), u = (256 s - k b) / (256 b + k s)
        const double c256 = 256.0;
        const int32_t k = cvttToInt32(std::fma(r, c256, 0.5));
        const double kd = static_cast<double>(k);
        const double num = std::fma(small, c256, -(kd * big));
        const double den = std::fma(small, kd, big * c256);
        const double u = num / den;
        const double u2 = u * u;
        r = u + bitsDouble(kAtanTable[k - 16]);
        r = std::fma(-(u2 * u), bitsDouble(0x3FD5555555550877ull), r);
    } else if (!(bitsDouble(0x3F1A36E2EB1C432Dull) > r)) { // RVA 0x33FAB: below 1e-4 keep r
        const double r2 = r * r;
        double p =
            std::fma(-r2, bitsDouble(0x3FC2492482BD6BE1ull), bitsDouble(0x3FC99999999643A3ull));
        p = std::fma(-r2, p, bitsDouble(0x3FD5555555555538ull));
        r = std::fma(-(r2 * r), p, r);
    }
    if (swapped)
        r = bitsDouble(0x3FF921FB54442D18ull) - r;
    if (xNeg)
        r = bitsDouble(0x400921FB54442D18ull) - r;
    if (yNeg)
        r = -r;
    return static_cast<float>(r);
}

namespace {
// ucrtbase RVA 0x10B590, 64 entries
constexpr uint64_t kExp2Table[64] = {
    0x3FF0000000000000ull, 0x3FF02C9A3E778061ull, 0x3FF059B0D3158574ull, 0x3FF0874518759BC8ull,
    0x3FF0B5586CF9890Full, 0x3FF0E3EC32D3D1A2ull, 0x3FF11301D0125B51ull, 0x3FF1429AAEA92DE0ull,
    0x3FF172B83C7D517Bull, 0x3FF1A35BEB6FCB75ull, 0x3FF1D4873168B9AAull, 0x3FF2063B88628CD6ull,
    0x3FF2387A6E756238ull, 0x3FF26B4565E27CDDull, 0x3FF29E9DF51FDEE1ull, 0x3FF2D285A6E4030Bull,
    0x3FF306FE0A31B715ull, 0x3FF33C08B26416FFull, 0x3FF371A7373AA9CBull, 0x3FF3A7DB34E59FF7ull,
    0x3FF3DEA64C123422ull, 0x3FF4160A21F72E2Aull, 0x3FF44E086061892Dull, 0x3FF486A2B5C13CD0ull,
    0x3FF4BFDAD5362A27ull, 0x3FF4F9B2769D2CA7ull, 0x3FF5342B569D4F82ull, 0x3FF56F4736B527DAull,
    0x3FF5AB07DD485429ull, 0x3FF5E76F15AD2148ull, 0x3FF6247EB03A5585ull, 0x3FF6623882552225ull,
    0x3FF6A09E667F3BCDull, 0x3FF6DFB23C651A2Full, 0x3FF71F75E8EC5F74ull, 0x3FF75FEB564267C9ull,
    0x3FF7A11473EB0187ull, 0x3FF7E2F336CF4E62ull, 0x3FF82589994CCE13ull, 0x3FF868D99B4492EDull,
    0x3FF8ACE5422AA0DBull, 0x3FF8F1AE99157736ull, 0x3FF93737B0CDC5E5ull, 0x3FF97D829FDE4E50ull,
    0x3FF9C49182A3F090ull, 0x3FFA0C667B5DE565ull, 0x3FFA5503B23E255Dull, 0x3FFA9E6B5579FDBFull,
    0x3FFAE89F995AD3ADull, 0x3FFB33A2B84F15FBull, 0x3FFB7F76F2FB5E47ull, 0x3FFBCC1E904BC1D2ull,
    0x3FFC199BDD85529Cull, 0x3FFC67F12E57D14Bull, 0x3FFCB720DCEF9069ull, 0x3FFD072D4A07897Cull,
    0x3FFD5818DCFBA487ull, 0x3FFDA9E603DB3285ull, 0x3FFDFC97337B9B5Full, 0x3FFE502EE78B3FF6ull,
    0x3FFEA4AFA2A490DAull, 0x3FFEFA1BEE615A27ull, 0x3FFF50765B6E4540ull, 0x3FFFA7C1819E90D8ull,
};
// ucrtbase RVA 0x109450, 129 entries
constexpr uint32_t kLogRcp[129] = {
    0x40000000u, 0x3FFE03F8u, 0x3FFC0FC1u, 0x3FFA232Du, 0x3FF83E10u, 0x3FF6603Eu, 0x3FF4898Du,
    0x3FF2B9D6u, 0x3FF0F0F1u, 0x3FEF2EB7u, 0x3FED7304u, 0x3FEBBDB3u, 0x3FEA0EA1u, 0x3FE865ACu,
    0x3FE6C2B4u, 0x3FE52598u, 0x3FE38E39u, 0x3FE1FC78u, 0x3FE07038u, 0x3FDEE95Cu, 0x3FDD67C9u,
    0x3FDBEB62u, 0x3FDA740Eu, 0x3FD901B2u, 0x3FD79436u, 0x3FD62B81u, 0x3FD4C77Bu, 0x3FD3680Du,
    0x3FD20D21u, 0x3FD0B6A0u, 0x3FCF6475u, 0x3FCE168Au, 0x3FCCCCCDu, 0x3FCB8728u, 0x3FCA4588u,
    0x3FC907DAu, 0x3FC7CE0Cu, 0x3FC6980Cu, 0x3FC565C8u, 0x3FC43730u, 0x3FC30C31u, 0x3FC1E4BCu,
    0x3FC0C0C1u, 0x3FBFA030u, 0x3FBE82FAu, 0x3FBD6910u, 0x3FBC5264u, 0x3FBB3EE7u, 0x3FBA2E8Cu,
    0x3FB92144u, 0x3FB81703u, 0x3FB70FBBu, 0x3FB60B61u, 0x3FB509E7u, 0x3FB40B41u, 0x3FB30F63u,
    0x3FB21643u, 0x3FB11FD4u, 0x3FB02C0Bu, 0x3FAF3ADEu, 0x3FAE4C41u, 0x3FAD602Bu, 0x3FAC7692u,
    0x3FAB8F6Au, 0x3FAAAAABu, 0x3FA9C84Au, 0x3FA8E83Fu, 0x3FA80A81u, 0x3FA72F05u, 0x3FA655C4u,
    0x3FA57EB5u, 0x3FA4A9CFu, 0x3FA3D70Au, 0x3FA3065Eu, 0x3FA237C3u, 0x3FA16B31u, 0x3FA0A0A1u,
    0x3F9FD80Au, 0x3F9F1166u, 0x3F9E4CADu, 0x3F9D89D9u, 0x3F9CC8E1u, 0x3F9C09C1u, 0x3F9B4C70u,
    0x3F9A90E8u, 0x3F99D723u, 0x3F991F1Au, 0x3F9868C8u, 0x3F97B426u, 0x3F97012Eu, 0x3F964FDAu,
    0x3F95A025u, 0x3F94F209u, 0x3F944581u, 0x3F939A86u, 0x3F92F114u, 0x3F924925u, 0x3F91A2B4u,
    0x3F90FDBCu, 0x3F905A38u, 0x3F8FB824u, 0x3F8F177Au, 0x3F8E7835u, 0x3F8DDA52u, 0x3F8D3DCBu,
    0x3F8CA29Cu, 0x3F8C08C1u, 0x3F8B7034u, 0x3F8AD8F3u, 0x3F8A42F8u, 0x3F89AE41u, 0x3F891AC7u,
    0x3F888889u, 0x3F87F781u, 0x3F8767ABu, 0x3F86D905u, 0x3F864B8Au, 0x3F85BF37u, 0x3F853408u,
    0x3F84A9FAu, 0x3F842108u, 0x3F839930u, 0x3F83126Fu, 0x3F828CC0u, 0x3F820821u, 0x3F81848Eu,
    0x3F810204u, 0x3F808081u, 0x3F800000u,
};
// ucrtbase RVA 0x108010, 129 entries
constexpr uint32_t kLogHi[129] = {
    0x00000000u, 0x3BFF0000u, 0x3C7E0000u, 0x3CBDC000u, 0x3CFC1000u, 0x3D1CF000u, 0x3D3BA000u,
    0x3D5A1000u, 0x3D785000u, 0x3D8B2000u, 0x3D9A0000u, 0x3DA8D000u, 0x3DB78000u, 0x3DC61000u,
    0x3DD49000u, 0x3DE2F000u, 0x3DF13000u, 0x3DFF6000u, 0x3E06B000u, 0x3E0DB000u, 0x3E14A000u,
    0x3E1B8000u, 0x3E226000u, 0x3E293000u, 0x3E2FF000u, 0x3E36B000u, 0x3E3D5000u, 0x3E43F000u,
    0x3E4A9000u, 0x3E511000u, 0x3E579000u, 0x3E5E1000u, 0x3E647000u, 0x3E6AE000u, 0x3E713000u,
    0x3E778000u, 0x3E7DC000u, 0x3E820000u, 0x3E851000u, 0x3E882000u, 0x3E8B3000u, 0x3E8E4000u,
    0x3E914000u, 0x3E944000u, 0x3E974000u, 0x3E9A3000u, 0x3E9D3000u, 0x3EA02000u, 0x3EA30000u,
    0x3EA5F000u, 0x3EA8D000u, 0x3EABB000u, 0x3EAE8000u, 0x3EB16000u, 0x3EB43000u, 0x3EB70000u,
    0x3EB9C000u, 0x3EBC9000u, 0x3EBF5000u, 0x3EC21000u, 0x3EC4D000u, 0x3EC78000u, 0x3ECA3000u,
    0x3ECCE000u, 0x3ECF9000u, 0x3ED24000u, 0x3ED4E000u, 0x3ED78000u, 0x3EDA2000u, 0x3EDCC000u,
    0x3EDF5000u, 0x3EE1E000u, 0x3EE47000u, 0x3EE70000u, 0x3EE99000u, 0x3EEC1000u, 0x3EEEA000u,
    0x3EF12000u, 0x3EF3A000u, 0x3EF61000u, 0x3EF89000u, 0x3EFB0000u, 0x3EFD7000u, 0x3EFFE000u,
    0x3F012000u, 0x3F025000u, 0x3F039000u, 0x3F04C000u, 0x3F05F000u, 0x3F072000u, 0x3F084000u,
    0x3F097000u, 0x3F0AA000u, 0x3F0BC000u, 0x3F0CF000u, 0x3F0E1000u, 0x3F0F4000u, 0x3F106000u,
    0x3F118000u, 0x3F12A000u, 0x3F13C000u, 0x3F14E000u, 0x3F160000u, 0x3F172000u, 0x3F183000u,
    0x3F195000u, 0x3F1A7000u, 0x3F1B8000u, 0x3F1C9000u, 0x3F1DB000u, 0x3F1EC000u, 0x3F1FD000u,
    0x3F20E000u, 0x3F21F000u, 0x3F230000u, 0x3F241000u, 0x3F252000u, 0x3F263000u, 0x3F273000u,
    0x3F284000u, 0x3F295000u, 0x3F2A5000u, 0x3F2B5000u, 0x3F2C6000u, 0x3F2D6000u, 0x3F2E6000u,
    0x3F2F7000u, 0x3F307000u, 0x3F317000u,
};
// ucrtbase RVA 0x108220, 129 entries
constexpr uint32_t kLogLo[129] = {
    0x00000000u, 0x3429AC41u, 0x35A8B0FCu, 0x368D83EAu, 0x361B0E78u, 0x3687B9FEu, 0x3631EC65u,
    0x36DD7119u, 0x35C30045u, 0x379B7751u, 0x37EBCB0Du, 0x37839F83u, 0x37528AE5u, 0x37A2EB18u,
    0x36DA7495u, 0x36A91EB7u, 0x3783B715u, 0x371131DBu, 0x383F3E68u, 0x38156A97u, 0x38297C0Fu,
    0x387E100Fu, 0x3815B665u, 0x37E5E3A1u, 0x38183853u, 0x35FE719Du, 0x38448108u, 0x38503290u,
    0x373539E8u, 0x385E0FF1u, 0x3864A740u, 0x3786742Du, 0x387BE3CDu, 0x3685AD3Eu, 0x3803B715u,
    0x37ADCBDCu, 0x380C36AFu, 0x371652D3u, 0x38927139u, 0x38C5FCD7u, 0x38AE55D5u, 0x3818C169u,
    0x38A0FDE7u, 0x38AD09EFu, 0x3862BAE1u, 0x38EECD4Cu, 0x3798AAD2u, 0x37421A1Au, 0x38C5E10Eu,
    0x37BF2AEEu, 0x382D872Du, 0x37EE2E8Au, 0x38DEDFACu, 0x3802F2B9u, 0x38481E9Bu, 0x380EAA2Bu,
    0x38EBFB5Du, 0x38255FDDu, 0x38783B82u, 0x3851DA1Eu, 0x374E1B05u, 0x388F439Bu, 0x38CA0E10u,
    0x38CAC08Bu, 0x3891F65Fu, 0x378121CBu, 0x386C9A9Au, 0x38949923u, 0x38777BCCu, 0x37B12D26u,
    0x38A6CED3u, 0x38EBD3E6u, 0x38FBE3CDu, 0x38D785C2u, 0x387E7E00u, 0x38F392C5u, 0x37D40983u,
    0x38081A7Cu, 0x3784C3ADu, 0x38CCE923u, 0x380F5FAFu, 0x3891FD38u, 0x38AC47BCu, 0x3897042Bu,
    0x392952D2u, 0x396FCED4u, 0x37F97073u, 0x385E9EAEu, 0x3865C84Au, 0x38130BA3u, 0x3979CF16u,
    0x3938CAC9u, 0x38C3D2F4u, 0x39755DECu, 0x38E6B467u, 0x395C0FB8u, 0x383EBCE0u, 0x38DCD192u,
    0x39186BDFu, 0x392DE74Cu, 0x392F0944u, 0x391BFF61u, 0x38E9ED44u, 0x38686DC8u, 0x396B99A7u,
    0x39099C89u, 0x37A27673u, 0x390BDAA3u, 0x397069ABu, 0x388449FFu, 0x39013538u, 0x392DC268u,
    0x3947F423u, 0x394FF17Cu, 0x3945E10Eu, 0x3929E8F5u, 0x38F85DB0u, 0x38735F99u, 0x396C08DBu,
    0x3909E600u, 0x37B4996Fu, 0x391233CCu, 0x397CEAD9u, 0x38ADB5CDu, 0x3920261Au, 0x3958EE36u,
    0x35AA4905u, 0x37CBD11Eu, 0x3805FDF4u,
};

// x86 cvtpd2dq with the default MXCSR rounding (nearest-even); only used where |v| < 2^31.
int32_t cvtRoundToInt32(double v) {
    return static_cast<int32_t>(std::nearbyint(v));
}
} // namespace

// expf: ucrtbase RVA 0xA84F0, FMA3 path. x*64/ln2 rounded to n, r = x - n*ln2/64 (fused),
// 2^(j/64) from a table, 2^m by building the exponent.
float crtExpf(float x) {
    const uint32_t bits = floatBits(x);
    const uint32_t ax = bits & 0x7FFFFFFFu;
    const double xd = static_cast<double>(x);
    if (ax >= 0x42B00000u) {     // RVA 0xA8590: |x| >= 88
        if (ax >= 0x7F800000u) { // RVA 0xA8620
            if (bits == 0x7F800000u)
                return x;
            if (bits == 0xFF800000u)
                return 0.0f;
            return bitsFloat(bits | 0x00400000u);
        }
        const double t = xd * bitsDouble(0x40571547652B82FEull);
        if (t >= 8192.0)
            return bitsFloat(0x7F800000u); // overflow handler RVA 0x31F50 returns +inf
        if (t < -9600.0)
            return 0.0f; // underflow handler returns 0
    }
    // RVA 0xA851D.
    const int32_t n = cvtRoundToInt32(xd * bitsDouble(0x40571547652B82FEull));
    const double r = std::fma(-static_cast<double>(n), bitsDouble(0x3F862E42FEFA39EFull), xd);
    const uint32_t j = static_cast<uint32_t>(n) & 63u;
    const int32_t m = (n - static_cast<int32_t>(j)) >> 6;
    const double p = std::fma(r, bitsDouble(0x3FC5555555555555ull), 0.5);
    double y = std::fma(r * r, p, r);
    const double tj = bitsDouble(kExp2Table[j]);
    y = std::fma(y, tj, tj);
    const uint64_t scale = (static_cast<uint64_t>(static_cast<uint32_t>(m)) + 0x3FF) << 52;
    return static_cast<float>(y * bitsDouble(scale));
}

// logf: ucrtbase RVA 0xA9AF0, FMA3 path, single precision with 129-entry tables.
float crtLogf(float x) {
    uint32_t bits = floatBits(x);
    if ((bits & 0x7FFFFFFFu) >= 0x7F800000u) { // RVA 0xA9C94
        if (bits == 0x7F800000u)
            return x;
        if (bits == 0xFF800000u)
            return bitsFloat(0xFFC00000u);
        return bitsFloat(bits | 0x00400000u);
    }
    if (!(x > 0.0f)) // RVA 0xA9C6A: log(+-0) = -inf, log(negative) = default NaN
        return bitsFloat(x == 0.0f ? 0xFF800000u : 0xFFC00000u);
    uint32_t mant = bits & 0x007FFFFFu;
    const uint32_t ex = bits >> 23;
    float e;
    float xf = x;
    if (ex == 0) { // RVA 0xA9C30: denormal, normalize through a float subtraction
        const float v = bitsFloat(mant | 0x3F800000u) - 1.0f;
        e = static_cast<float>(static_cast<int32_t>(floatBits(v) >> 23) - 0xFD);
        mant = floatBits(v) & 0x007FFFFFu;
        xf = bitsFloat(mant);
    } else {
        e = static_cast<float>(static_cast<int32_t>(ex) - 0x7F);
    }
    const float f1 = xf - 1.0f;
    if (std::fabs(f1) < bitsFloat(0x3D800000u)) { // RVA 0xA9BE0: |x - 1| < 1/16
        const float u = f1 / (2.0f + f1);
        const float h = f1 * u;
        const float v = u + u;
        const float v2 = v * v;
        const float v3 = v * v2;
        const float q = std::fma(bitsFloat(0x3C4CCCCDu), v2, bitsFloat(0x3DAAAAABu));
        return f1 + std::fma(v3, q, -h);
    }
    // RVA 0xA9B5A.
    const float m = bitsFloat(mant | 0x3F000000u);
    const uint32_t idx = (mant >> 16) + ((mant >> 15) & 1u);
    const float big = bitsFloat((idx << 16) | 0x3F000000u);
    const float r = (big - m) * bitsFloat(kLogRcp[idx]);
    const float p = std::fma(r, bitsFloat(0x3EAAAAABu), 0.5f);
    const float s = std::fma(p, r * r, r);
    float lo = std::fma(e, bitsFloat(0x3805FDF4u), -s);
    const float hi = std::fma(e, bitsFloat(0x3F317000u), bitsFloat(kLogHi[idx]));
    lo = lo + bitsFloat(kLogLo[idx]);
    return hi + lo;
}

namespace {
// ucrtbase RVA 0x109660, 257 entries
constexpr uint64_t kPowRcp[257] = {
    0x4000000000000000ull, 0x3FFFE01FE01FE020ull, 0x3FFFC07F01FC07F0ull, 0x3FFFA11CAA01FA12ull,
    0x3FFF81F81F81F820ull, 0x3FFF6310ACA0DBB5ull, 0x3FFF44659E4A4271ull, 0x3FFF25F644230AB5ull,
    0x3FFF07C1F07C1F08ull, 0x3FFEE9C7F8458E02ull, 0x3FFECC07B301ECC0ull, 0x3FFEAE807ABA01EBull,
    0x3FFE9131ABF0B767ull, 0x3FFE741AA59750E4ull, 0x3FFE573AC901E574ull, 0x3FFE3A9179DC1A73ull,
    0x3FFE1E1E1E1E1E1Eull, 0x3FFE01E01E01E01Eull, 0x3FFDE5D6E3F8868Aull, 0x3FFDCA01DCA01DCAull,
    0x3FFDAE6076B981DBull, 0x3FFD92F2231E7F8Aull, 0x3FFD77B654B82C34ull, 0x3FFD5CAC807572B2ull,
    0x3FFD41D41D41D41Dull, 0x3FFD272CA3FC5B1Aull, 0x3FFD0CB58F6EC074ull, 0x3FFCF26E5C44BFC6ull,
    0x3FFCD85689039B0Bull, 0x3FFCBE6D9601CBE7ull, 0x3FFCA4B3055EE191ull, 0x3FFC8B265AFB8A42ull,
    0x3FFC71C71C71C71Cull, 0x3FFC5894D10D4986ull, 0x3FFC3F8F01C3F8F0ull, 0x3FFC26B5392EA01Cull,
    0x3FFC0E070381C0E0ull, 0x3FFBF583EE868D8Bull, 0x3FFBDD2B899406F7ull, 0x3FFBC4FD65883E7Bull,
    0x3FFBACF914C1BAD0ull, 0x3FFB951E2B18FF23ull, 0x3FFB7D6C3DDA338Bull, 0x3FFB65E2E3BEEE05ull,
    0x3FFB4E81B4E81B4Full, 0x3FFB37484AD806CEull, 0x3FFB2036406C80D9ull, 0x3FFB094B31D922A4ull,
    0x3FFAF286BCA1AF28ull, 0x3FFADBE87F94905Eull, 0x3FFAC5701AC5701Bull, 0x3FFAAF1D2F87EBFDull,
    0x3FFA98EF606A63BEull, 0x3FFA82E65130E159ull, 0x3FFA6D01A6D01A6Dull, 0x3FFA574107688A4Aull,
    0x3FFA41A41A41A41Aull, 0x3FFA2C2A87C51CA0ull, 0x3FFA16D3F97A4B02ull, 0x3FFA01A01A01A01Aull,
    0x3FF9EC8E951033D9ull, 0x3FF9D79F176B682Dull, 0x3FF9C2D14EE4A102ull, 0x3FF9AE24EA5510DAull,
    0x3FF999999999999Aull, 0x3FF9852F0D8EC0FFull, 0x3FF970E4F80CB872ull, 0x3FF95CBB0BE377AEull,
    0x3FF948B0FCD6E9E0ull, 0x3FF934C67F9B2CE6ull, 0x3FF920FB49D0E229ull, 0x3FF90D4F120190D5ull,
    0x3FF8F9C18F9C18FAull, 0x3FF8E6527AF1373Full, 0x3FF8D3018D3018D3ull, 0x3FF8BFCE8062FF3Aull,
    0x3FF8ACB90F6BF3AAull, 0x3FF899C0F601899Cull, 0x3FF886E5F0ABB04Aull, 0x3FF87427BCC092B9ull,
    0x3FF8618618618618ull, 0x3FF84F00C2780614ull, 0x3FF83C977AB2BEDDull, 0x3FF82A4A0182A4A0ull,
    0x3FF8181818181818ull, 0x3FF8060180601806ull, 0x3FF7F405FD017F40ull, 0x3FF7E225515A4F1Dull,
    0x3FF7D05F417D05F4ull, 0x3FF7BEB3922E017Cull, 0x3FF7AD2208E0ECC3ull, 0x3FF79BAA6BB6398Bull,
    0x3FF78A4C8178A4C8ull, 0x3FF77908119AC60Dull, 0x3FF767DCE434A9B1ull, 0x3FF756CAC201756Dull,
    0x3FF745D1745D1746ull, 0x3FF734F0C541FE8Dull, 0x3FF724287F46DEBCull, 0x3FF713786D9C7C09ull,
    0x3FF702E05C0B8170ull, 0x3FF6F26016F26017ull, 0x3FF6E1F76B4337C7ull, 0x3FF6D1A62681C861ull,
    0x3FF6C16C16C16C17ull, 0x3FF6B1490AA31A3Dull, 0x3FF6A13CD1537290ull, 0x3FF691473A88D0C0ull,
    0x3FF6816816816817ull, 0x3FF6719F3601671Aull, 0x3FF661EC6A5122F9ull, 0x3FF6524F853B4AA3ull,
    0x3FF642C8590B2164ull, 0x3FF63356B88AC0DEull, 0x3FF623FA77016240ull, 0x3FF614B36831AE94ull,
    0x3FF6058160581606ull, 0x3FF5F66434292DFCull, 0x3FF5E75BB8D015E7ull, 0x3FF5D867C3ECE2A5ull,
    0x3FF5C9882B931057ull, 0x3FF5BABCC647FA91ull, 0x3FF5AC056B015AC0ull, 0x3FF59D61F123CCAAull,
    0x3FF58ED2308158EDull, 0x3FF5805601580560ull, 0x3FF571ED3C506B3Aull, 0x3FF56397BA7C52E2ull,
    0x3FF5555555555555ull, 0x3FF54725E6BB82FEull, 0x3FF5390948F40FEBull, 0x3FF52AFF56A8054Bull,
    0x3FF51D07EAE2F815ull, 0x3FF50F22E111C4C5ull, 0x3FF5015015015015ull, 0x3FF4F38F62DD4C9Bull,
    0x3FF4E5E0A72F0539ull, 0x3FF4D843BEDC2C4Cull, 0x3FF4CAB88725AF6Eull, 0x3FF4BD3EDDA68FE1ull,
    0x3FF4AFD6A052BF5Bull, 0x3FF4A27FAD76014Aull, 0x3FF49539E3B2D067ull, 0x3FF4880522014880ull,
    0x3FF47AE147AE147Bull, 0x3FF46DCE34596066ull, 0x3FF460CBC7F5CF9Aull, 0x3FF453D9E2C776CAull,
    0x3FF446F86562D9FBull, 0x3FF43A2730ABEE4Dull, 0x3FF42D6625D51F87ull, 0x3FF420B5265E5951ull,
    0x3FF4141414141414ull, 0x3FF40782D10E6566ull, 0x3FF3FB013FB013FBull, 0x3FF3EE8F42A5AF07ull,
    0x3FF3E22CBCE4A902ull, 0x3FF3D5D991AA75C6ull, 0x3FF3C995A47BABE7ull, 0x3FF3BD60D9232955ull,
    0x3FF3B13B13B13B14ull, 0x3FF3A524387AC822ull, 0x3FF3991C2C187F63ull, 0x3FF38D22D366088Eull,
    0x3FF3813813813814ull, 0x3FF3755BD1C945EEull, 0x3FF3698DF3DE0748ull, 0x3FF35DCE5F9F2AF8ull,
    0x3FF3521CFB2B78C1ull, 0x3FF34679ACE01346ull, 0x3FF33AE45B57BCB2ull, 0x3FF32F5CED6A1DFAull,
    0x3FF323E34A2B10BFull, 0x3FF3187758E9EBB6ull, 0x3FF30D190130D190ull, 0x3FF301C82AC40260ull,
    0x3FF2F684BDA12F68ull, 0x3FF2EB4EA1FED14Bull, 0x3FF2E025C04B8097ull, 0x3FF2D50A012D50A0ull,
    0x3FF2C9FB4D812CA0ull, 0x3FF2BEF98E5A3711ull, 0x3FF2B404AD012B40ull, 0x3FF2A91C92F3C105ull,
    0x3FF29E4129E4129Eull, 0x3FF293725BB804A5ull, 0x3FF288B01288B013ull, 0x3FF27DFA38A1CE4Dull,
    0x3FF27350B8812735ull, 0x3FF268B37CD60127ull, 0x3FF25E22708092F1ull, 0x3FF2539D7E9177B2ull,
    0x3FF2492492492492ull, 0x3FF23EB79717605Bull, 0x3FF23456789ABCDFull, 0x3FF22A0122A0122Aull,
    0x3FF21FB78121FB78ull, 0x3FF21579804855E6ull, 0x3FF20B470C67C0D9ull, 0x3FF2012012012012ull,
    0x3FF1F7047DC11F70ull, 0x3FF1ECF43C7FB84Cull, 0x3FF1E2EF3B3FB874ull, 0x3FF1D8F5672E4ABDull,
    0x3FF1CF06ADA2811Dull, 0x3FF1C522FC1CE059ull, 0x3FF1BB4A4046ED29ull, 0x3FF1B17C67F2BAE3ull,
    0x3FF1A7B9611A7B96ull, 0x3FF19E0119E0119Eull, 0x3FF19453808CA29Cull, 0x3FF18AB083902BDBull,
    0x3FF1811811811812ull, 0x3FF1778A191BD684ull, 0x3FF16E0689427379ull, 0x3FF1648D50FC3201ull,
    0x3FF15B1E5F75270Dull, 0x3FF151B9A3FDD5C9ull, 0x3FF1485F0E0ACD3Bull, 0x3FF13F0E8D344724ull,
    0x3FF135C81135C811ull, 0x3FF12C8B89EDC0ACull, 0x3FF12358E75D3033ull, 0x3FF11A3019A74826ull,
    0x3FF1111111111111ull, 0x3FF107FBBE011080ull, 0x3FF0FEF010FEF011ull, 0x3FF0F5EDFAB325A2ull,
    0x3FF0ECF56BE69C90ull, 0x3FF0E40655826011ull, 0x3FF0DB20A88F4696ull, 0x3FF0D24456359E3Aull,
    0x3FF0C9714FBCDA3Bull, 0x3FF0C0A7868B4171ull, 0x3FF0B7E6EC259DC8ull, 0x3FF0AF2F722EECB5ull,
    0x3FF0A6810A6810A7ull, 0x3FF09DDBA6AF8360ull, 0x3FF0953F39010954ull, 0x3FF08CABB37565E2ull,
    0x3FF0842108421084ull, 0x3FF07B9F29B8EAE2ull, 0x3FF073260A47F7C6ull, 0x3FF06AB59C7912FBull,
    0x3FF0624DD2F1A9FCull, 0x3FF059EEA0727586ull, 0x3FF05197F7D73404ull, 0x3FF04949CC1664C5ull,
    0x3FF0410410410410ull, 0x3FF038C6B78247FCull, 0x3FF03091B51F5E1Aull, 0x3FF02864FC7729E9ull,
    0x3FF0204081020408ull, 0x3FF0182436517A37ull, 0x3FF0101010101010ull, 0x3FF0080402010080ull,
    0x3FF0000000000000ull,
};
// ucrtbase RVA 0x10A070, 257 entries
constexpr uint64_t kPowLog[257] = {
    0x0000000000000000ull, 0x3F6FF00AA2B10BC0ull, 0x3F7FE02A6B106789ull, 0x3F87DC475F810A77ull,
    0x3F8FC0A8B0FC03E4ull, 0x3F93CEA44346A575ull, 0x3F97B91B07D5B11Bull, 0x3F9B9FC027AF9198ull,
    0x3F9F829B0E783300ull, 0x3FA1B0D98923D980ull, 0x3FA39E87B9FEBD60ull, 0x3FA58A5BAFC8E4D5ull,
    0x3FA77458F632DCFCull, 0x3FA95C830EC8E3EBull, 0x3FAB42DD711971BFull, 0x3FAD276B8ADB0B52ull,
    0x3FAF0A30C01162A6ull, 0x3FB075983598E471ull, 0x3FB16536EEA37AE1ull, 0x3FB253F62F0A1417ull,
    0x3FB341D7961BD1D1ull, 0x3FB42EDCBEA646F0ull, 0x3FB51B073F06183Full, 0x3FB60658A93750C4ull,
    0x3FB6F0D28AE56B4Cull, 0x3FB7DA766D7B12CDull, 0x3FB8C345D6319B21ull, 0x3FB9AB42462033ADull,
    0x3FBA926D3A4AD563ull, 0x3FBB78C82BB0EDA1ull, 0x3FBC5E548F5BC743ull, 0x3FBD4313D66CB35Dull,
    0x3FBE27076E2AF2E6ull, 0x3FBF0A30C01162A6ull, 0x3FBFEC9131DBEABBull, 0x3FC0671512CA596Eull,
    0x3FC0D77E7CD08E59ull, 0x3FC14785846742ACull, 0x3FC1B72AD52F67A0ull, 0x3FC2266F190A5ACBull,
    0x3FC29552F81FF523ull, 0x3FC303D718E47FD3ull, 0x3FC371FC201E8F74ull, 0x3FC3DFC2B0ECC62Aull,
    0x3FC44D2B6CCB7D1Eull, 0x3FC4BA36F39A55E5ull, 0x3FC526E5E3A1B438ull, 0x3FC59338D9982086ull,
    0x3FC5FF3070A793D4ull, 0x3FC66ACD4272AD51ull, 0x3FC6D60FE719D21Dull, 0x3FC740F8F54037A5ull,
    0x3FC7AB890210D909ull, 0x3FC815C0A14357EBull, 0x3FC87FA06520C911ull, 0x3FC8E928DE886D41ull,
    0x3FC9525A9CF456B4ull, 0x3FC9BB362E7DFB83ull, 0x3FCA23BC1FE2B563ull, 0x3FCA8BECFC882F19ull,
    0x3FCAF3C94E80BFF3ull, 0x3FCB5B519E8FB5A4ull, 0x3FCBC286742D8CD6ull, 0x3FCC2968558C18C1ull,
    0x3FCC8FF7C79A9A22ull, 0x3FCCF6354E09C5DCull, 0x3FCD5C216B4FBB91ull, 0x3FCDC1BCA0ABEC7Dull,
    0x3FCE27076E2AF2E6ull, 0x3FCE8C0252AA5A60ull, 0x3FCEF0ADCBDC5936ull, 0x3FCF550A564B7B37ull,
    0x3FCFB9186D5E3E2Bull, 0x3FD00E6C45AD501Dull, 0x3FD0402594B4D041ull, 0x3FD071B85FCD590Dull,
    0x3FD0A324E27390E3ull, 0x3FD0D46B579AB74Bull, 0x3FD1058BF9AE4AD5ull, 0x3FD136870293A8B0ull,
    0x3FD1675CABABA60Eull, 0x3FD1980D2DD4236Full, 0x3FD1C898C16999FBull, 0x3FD1F8FF9E48A2F3ull,
    0x3FD22941FBCF7966ull, 0x3FD2596010DF763Aull, 0x3FD2895A13DE86A3ull, 0x3FD2B9303AB89D25ull,
    0x3FD2E8E2BAE11D31ull, 0x3FD31871C9544185ull, 0x3FD347DD9A987D55ull, 0x3FD3772662BFD85Bull,
    0x3FD3A64C556945EAull, 0x3FD3D54FA5C1F710ull, 0x3FD404308686A7E4ull, 0x3FD432EF2A04E814ull,
    0x3FD4618BC21C5EC2ull, 0x3FD49006804009D1ull, 0x3FD4BE5F957778A1ull, 0x3FD4EC973260026Aull,
    0x3FD51AAD872DF82Dull, 0x3FD548A2C3ADD263ull, 0x3FD5767717455A6Cull, 0x3FD5A42AB0F4CFE2ull,
    0x3FD5D1BDBF5809CAull, 0x3FD5FF3070A793D4ull, 0x3FD62C82F2B9C795ull, 0x3FD659B57303E1F3ull,
    0x3FD686C81E9B14AFull, 0x3FD6B3BB2235943Eull, 0x3FD6E08EAA2BA1E4ull, 0x3FD70D42E2789236ull,
    0x3FD739D7F6BBD007ull, 0x3FD7664E1239DBCFull, 0x3FD792A55FDD47A2ull, 0x3FD7BEDE0A37AFC0ull,
    0x3FD7EAF83B82AFC3ull, 0x3FD816F41DA0D496ull, 0x3FD842D1DA1E8B17ull, 0x3FD86E919A330BA0ull,
    0x3FD89A3386C1425Bull, 0x3FD8C5B7C858B48Bull, 0x3FD8F11E873662C7ull, 0x3FD91C67EB45A83Eull,
    0x3FD947941C2116FBull, 0x3FD972A341135158ull, 0x3FD99D958117E08Bull, 0x3FD9C86B02DC0863ull,
    0x3FD9F323ECBF984Cull, 0x3FDA1DC064D5B995ull, 0x3FDA484090E5BB0Aull, 0x3FDA72A4966BD9EAull,
    0x3FDA9CEC9A9A084Aull, 0x3FDAC718C258B0E4ull, 0x3FDAF1293247786Bull, 0x3FDB1B1E0EBDFC5Bull,
    0x3FDB44F77BCC8F63ull, 0x3FDB6EB59D3CF35Eull, 0x3FDB9858969310FBull, 0x3FDBC1E08B0DAD0Aull,
    0x3FDBEB4D9DA71B7Cull, 0x3FDC149FF115F027ull, 0x3FDC3DD7A7CDAD4Dull, 0x3FDC66F4E3FF6FF8ull,
    0x3FDC8FF7C79A9A22ull, 0x3FDCB8E0744D7ACAull, 0x3FDCE1AF0B85F3EBull, 0x3FDD0A63AE721E64ull,
    0x3FDD32FE7E00EBD5ull, 0x3FDD5B7F9AE2C684ull, 0x3FDD83E7258A2F3Eull, 0x3FDDAC353E2C5954ull,
    0x3FDDD46A04C1C4A1ull, 0x3FDDFC859906D5B5ull, 0x3FDE24881A7C6C26ull, 0x3FDE4C71A8687704ull,
    0x3FDE744261D68788ull, 0x3FDE9BFA659861F5ull, 0x3FDEC399D2468CC0ull, 0x3FDEEB20C640DDF4ull,
    0x3FDF128F5FAF06EDull, 0x3FDF39E5BC811E5Cull, 0x3FDF6123FA7028ACull, 0x3FDF884A36FE9EC2ull,
    0x3FDFAF588F78F31Full, 0x3FDFD64F20F61572ull, 0x3FDFFD2E0857F498ull, 0x3FE011FAB125FF8Aull,
    0x3FE02552A5A5D0FFull, 0x3FE0389EEFCE633Bull, 0x3FE04BDF9DA926D2ull, 0x3FE05F14BD26459Cull,
    0x3FE0723E5C1CDF40ull, 0x3FE0855C884B450Eull, 0x3FE0986F4F573521ull, 0x3FE0AB76BECE14D2ull,
    0x3FE0BE72E4252A83ull, 0x3FE0D163CCB9D6B8ull, 0x3FE0E44985D1CC8Cull, 0x3FE0F7241C9B497Dull,
    0x3FE109F39E2D4C97ull, 0x3FE11CB81787CCF8ull, 0x3FE12F719593EFBCull, 0x3FE1422025243D45ull,
    0x3FE154C3D2F4D5EAull, 0x3FE1675CABABA60Eull, 0x3FE179EABBD899A1ull, 0x3FE18C6E0FF5CF06ull,
    0x3FE19EE6B467C96Full, 0x3FE1B154B57DA29Full, 0x3FE1C3B81F713C25ull, 0x3FE1D610FE677003ull,
    0x3FE1E85F5E7040D0ull, 0x3FE1FAA34B87094Cull, 0x3FE20CDCD192AB6Eull, 0x3FE21F0BFC65BEECull,
    0x3FE23130D7BEBF43ull, 0x3FE2434B6F483934ull, 0x3FE2555BCE98F7CBull, 0x3FE26762013430E0ull,
    0x3FE2795E1289B11Bull, 0x3FE28B500DF60783ull, 0x3FE29D37FEC2B08Bull, 0x3FE2AF15F02640ADull,
    0x3FE2C0E9ED448E8Cull, 0x3FE2D2B4012EDC9Eull, 0x3FE2E47436E40268ull, 0x3FE2F62A99509546ull,
    0x3FE307D7334F10BEull, 0x3FE3197A0FA7FE6Aull, 0x3FE32B1339121D71ull, 0x3FE33CA2BA328995ull,
    0x3FE34E289D9CE1D3ull, 0x3FE35FA4EDD36EA0ull, 0x3FE37117B54747B6ull, 0x3FE38280FE58797Full,
    0x3FE393E0D3562A1Aull, 0x3FE3A5373E7EBDFAull, 0x3FE3B68449FFFC23ull, 0x3FE3C7C7FFF73206ull,
    0x3FE3D9026A7156FBull, 0x3FE3EA33936B2F5Cull, 0x3FE3FB5B84D16F42ull, 0x3FE40C7A4880DCE9ull,
    0x3FE41D8FE84672AEull, 0x3FE42E9C6DDF80BFull, 0x3FE43F9FE2F9CE67ull, 0x3FE4509A5133BB0Aull,
    0x3FE4618BC21C5EC2ull, 0x3FE472743F33AAADull, 0x3FE48353D1EA88DFull, 0x3FE4942A83A2FC07ull,
    0x3FE4A4F85DB03EBBull, 0x3FE4B5BD6956E274ull, 0x3FE4C679AFCCEE3Aull, 0x3FE4D72D3A39FD00ull,
    0x3FE4E7D811B75BB1ull, 0x3FE4F87A3F5026E9ull, 0x3FE50913CC01686Bull, 0x3FE519A4C0BA3446ull,
    0x3FE52A2D265BC5ABull, 0x3FE53AAD05B99B7Dull, 0x3FE54B2467999498ull, 0x3FE55B9354B40BCDull,
    0x3FE56BF9D5B3F399ull, 0x3FE57C57F336F191ull, 0x3FE58CADB5CD7989ull, 0x3FE59CFB25FAE87Eull,
    0x3FE5AD404C359F2Dull, 0x3FE5BD7D30E71C73ull, 0x3FE5CDB1DC6C1765ull, 0x3FE5DDDE57149923ull,
    0x3FE5EE02A9241675ull, 0x3FE5FE1EDAD18919ull, 0x3FE60E32F44788D9ull, 0x3FE61E3EFDA46467ull,
    0x3FE62E42FEFA39EFull,
};

float quietNan(uint32_t bits) {
    return bitsFloat(bits | 0x00400000u);
}

// "y is an odd integer" test of powf (RVA 0xAA7E6 and siblings): |y| >= 2^24 is even; otherwise
// roundss(y) must equal y and the low bit of cvtss2si(y) decides.
bool powOddInteger(float y) {
    if ((floatBits(y) & 0x7F800000u) > 0x4B000000u)
        return false;
    if (std::nearbyint(y) != y)
        return false;
    return (static_cast<int32_t>(y) & 1) != 0;
}

bool powIsInteger(float y) {
    return (floatBits(y) & 0x7F800000u) > 0x4B000000u || std::nearbyint(y) == y;
}

// Final exp step of powf. fused = FMA3 code (RVA 0xAA589); otherwise the SSE2 code at RVA 0xA9DEE,
// which the FMA3 path jumps into after its |x - 1| < 1/16 log (RVA 0xAA6EB).
float powExp(double w, uint32_t sign, bool fused) {
    if (w > bitsDouble(0x40562E4300000000ull))
        return bitsFloat(0x7F800000u | sign); // RVA 0xAAB30, handler returns the value
    if (!(w > bitsDouble(0xC059D1DA00000000ull)))
        return bitsFloat(sign); // RVA 0xAAB00
    const int32_t n = cvtRoundToInt32(w * bitsDouble(0x40571547652B82FEull));
    const double nd = static_cast<double>(n);
    const double tj = bitsDouble(kExp2Table[static_cast<uint32_t>(n) & 63u]);
    double z;
    if (fused) {
        const double r = std::fma(-nd, bitsDouble(0x3F862E42FEFA39EFull), w);
        double q = std::fma(r, bitsDouble(0x3FC5555555555555ull), 0.5);
        q = std::fma(r, q, 1.0);
        z = std::fma(r * q, tj, tj);
    } else {
        const double r = w - nd * bitsDouble(0x3F862E42FEFA39EFull);
        const double q = bitsDouble(0x3FC5555555555555ull) * r + 0.5;
        z = (r * r) * q + r;
        z = z * tj + tj;
    }
    const uint64_t scaled =
        doubleBits(z) + (static_cast<uint64_t>(static_cast<int64_t>(n >> 6)) << 52);
    return bitsFloat(floatBits(static_cast<float>(bitsDouble(scaled))) | sign);
}

} // namespace

// powf: ucrtbase RVA 0xA9CD0, FMA3 path 0xAA486. log2-free scheme: log(|x|) in double from a
// 257-entry table, times y, then the expf-style exp. Special cases follow the branch order of
// the binary; error-handler calls (RVA 0x31FF0) return the value they are given (codes 3..5 quiet).
float crtPowf(float x, float y) {
    const uint32_t xb = floatBits(x), yb = floatBits(y);
    const uint32_t ax = xb & 0x7FFFFFFFu, ay = yb & 0x7FFFFFFFu;
    if (ay >= 0x7F800000u) {    // RVA 0xAA830
        if (ay > 0x7F800000u) { // y NaN
            if (ax > 0x7F800000u)
                return quietNan(xb == 0xFFC00000u ? yb : xb);
            if (xb == 0x3F800000u)
                return ay >= 0x7FC00000u ? 1.0f : quietNan(0x3F800000u); // RVA 0xAA970
            return quietNan(yb);
        }
        if (ax > 0x7F800000u)
            return quietNan(xb);
        if (ax == 0x3F800000u)
            return 1.0f;
        const bool small = ax < 0x3F800000u;
        if (!(yb >> 31))
            return bitsFloat(small ? 0u : 0x7F800000u);
        return bitsFloat(small ? 0x7F800000u : 0u);
    }
    if (ay <= 0x3F800000u) { // RVA 0xAA8D0
        if (ay == 0) {
            if (ax <= 0x7F800000u || ax >= 0x7FC00000u)
                return 1.0f;
            return quietNan(ax);
        }
        if (yb == 0x3F800000u)
            return ax <= 0x7F800000u ? x : quietNan(xb);
    }
    if (ax >= 0x7F800000u) { // RVA 0xAA790
        if (xb == 0x7F800000u)
            return bitsFloat(static_cast<int32_t>(yb) < 0 ? 0u : 0x7F800000u);
        if (xb == 0xFF800000u) {
            const uint32_t mag = static_cast<int32_t>(yb) < 0 ? 0u : 0x7F800000u;
            return bitsFloat(mag | (powOddInteger(y) ? 0x80000000u : 0u));
        }
        return quietNan(xb);
    }
    const double xd = static_cast<double>(x);
    const double yd = static_cast<double>(y);
    uint32_t sign = 0;
    if (static_cast<int32_t>(xb) < 0x3F880000) { // RVA 0xAA620
        if (static_cast<int32_t>(xb) <= 0) {
            if (ax == 0) { // RVA 0xAA6F0: zero base
                const uint32_t mag = static_cast<int32_t>(yb) < 0 ? 0x7F800000u : 0u;
                return bitsFloat(mag | (powOddInteger(y) ? (xb & 0x80000000u) : 0u));
            }
            if ((ay & 0x7F800000u) <= 0x4B000000u) {
                if (!powIsInteger(y))
                    return bitsFloat(0xFFC00000u); // RVA 0xAAB60: negative base, non-integer y
                if (powOddInteger(y))
                    sign = 0x80000000u;
            }
        }
        const double d = xd - 1.0;
        if ((doubleBits(d) & 0x7FFFFFFFFFFFFFFFull) < 0x3FB0000000000000ull) {
            // RVA 0xAA67F: log(x) = d - u*d + v^3*A + v^7*B, u = d/(d+2), v = 2u (no fusing).
            const double u = d / (d + 2.0);
            const double ud = u * d;
            const double v = u + u;
            const double v2 = v * v;
            const double a =
                v2 * bitsDouble(0x3F89999999BAC6D4ull) + bitsDouble(0x3FB55555555554E6ull);
            const double b =
                v2 * bitsDouble(0x3F3C8034C85DFFF0ull) + bitsDouble(0x3F62492307F1519Full);
            const double v3 = v * v2;
            const double v7 = (v2 * v2) * v3;
            const double logx = d + ((v7 * b + v3 * a) - ud);
            return powExp(yd * logx, sign, false);
        }
    }
    // RVA 0xAA4D7: |x| = 2^e * 2m, m in [0.5, 1); F = nearest 1/256 step.
    const uint64_t xbits = doubleBits(xd);
    const uint64_t mant = xbits & 0x000FFFFFFFFFFFFFull;
    const uint32_t idx =
        static_cast<uint32_t>(mant >> 44) + static_cast<uint32_t>((mant >> 43) & 1);
    const double f = bitsDouble(static_cast<uint64_t>(idx | 0x3FE00u) << 44);
    const double m = bitsDouble(mant | 0x3FE0000000000000ull);
    const double e = static_cast<double>(static_cast<int32_t>(((xbits >> 52) & 0x7FF) - 0x3FF));
    const double r = (f - m) * bitsDouble(kPowRcp[idx]);
    double p = std::fma(r, bitsDouble(0x3FD5555555555555ull), 0.5);
    p = std::fma(r, p, 1.0);
    const double logx = (e * bitsDouble(0x3FE62E42FEFA39EFull) + bitsDouble(kPowLog[idx])) - r * p;
    return powExp(yd * logx, sign, true);
}

// fmodf: ucrtbase RVA 0x32440 (no FMA path). The remainder of two floats is exact in double, so
// once the special cases are handled the IEEE fmod gives the DLL's bits.
float crtFmodf(float x, float y) {
    const uint32_t xb = floatBits(x), yb = floatBits(y);
    const uint32_t ex = (xb >> 23) & 0xFFu, ey = (yb >> 23) & 0xFFu;
    const bool xZero = (xb & 0x7FFFFFFFu) == 0, yZero = (yb & 0x7FFFFFFFu) == 0;
    if (ex == 0xFF) // RVA 0x3261D: x inf or NaN
        return (xb & 0x007FFFFFu) ? quietNan(xb) : bitsFloat(0xFFC00000u);
    if (ey == 0xFF) // RVA 0x32745: y inf or NaN
        return (yb & 0x007FFFFFu) ? quietNan(yb) : x;
    if (yZero) // RVA 0x32559 / 0x32699
        return bitsFloat(0xFFC00000u);
    if (xZero)
        return x; // RVA 0x32650
    return static_cast<float>(std::fmod(static_cast<double>(x), static_cast<double>(y)));
}

// floorf / ceilf: ucrtbase RVA 0xF7050 -> 0xA8670 (roundss mode 9) and 0xF7030 -> 0xA8690
// (roundss mode 10) on SSE4.1 CPUs: plain IEEE rounding, NaN quieted.
float crtFloorf(float x) {
    if ((floatBits(x) & 0x7FFFFFFFu) > 0x7F800000u)
        return quietNan(floatBits(x));
    return std::floor(x);
}

float crtCeilf(float x) {
    if ((floatBits(x) & 0x7FFFFFFFu) > 0x7F800000u)
        return quietNan(floatBits(x));
    return std::ceil(x);
}

// sqrtf: ucrtbase RVA 0x32100: sqrtss, NaN quieted, negative (non-zero) -> default NaN.
float crtSqrtf(float x) {
    const uint32_t b = floatBits(x);
    if ((b & 0x7FFFFFFFu) > 0x7F800000u)
        return quietNan(b);
    if ((b >> 31) && (b & 0x7FFFFFFFu))
        return bitsFloat(0xFFC00000u);
    return std::sqrt(x);
}

// Every function above is ported; nothing falls back to libm any more. Kept so harnesses can
// assert it stays zero.
unsigned long long crtInexactCalls() {
    return 0;
}

} // namespace rlios::sim
