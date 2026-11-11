// The math functions the physics path may call, reproduced so every platform returns the bits the
// Windows game computes. The game imports them from the Microsoft UCRT (api-ms-win-crt-math-l1-1-0
// -> its app-local ucrtbase.dll 10.0.26100.2454, FMA3 code path); libm must never be used for these
// in sim/. Every function is a port of the DLL's FMA3 code path and is proven bit-exact against the
// real DLL by tools/harness/ucrt_check.sh (1-argument functions: all 2^32 inputs; 2-argument
// functions: structured exponent/special sweep plus >= 10^9 random pairs). NaN results reproduce
// the DLL's bits too (quieted inputs, default NaN 0xFFC00000).
#pragma once

#include <cstdint>

namespace rlios::sim {

float crtSinf(float x);
float crtCosf(float x);
bool crtTrigInRange(float x); // |x| <= pi/4 (no reduction needed); every input is exact anyway

float crtTanf(float x);
float crtAsinf(float x);
float crtAcosf(float x);
float crtAtanf(float x);
float crtAtan2f(float y, float x);
float crtExpf(float x);
float crtLogf(float x);
float crtPowf(float x, float y);
float crtFmodf(float x, float y);
float crtFloorf(float x);
float crtCeilf(float x);
float crtSqrtf(float x);

// Number of calls that took a path NOT proven bit-exact. Always 0 now that every function is
// ported; kept so harnesses (ball_harness) can report/assert it. Diagnostic only, never read by
// the sim.
unsigned long long crtInexactCalls();


} // namespace rlios::sim
