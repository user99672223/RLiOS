// The transcendental functions the physics path calls, reproduced so every platform returns the
// bits the Windows game computes. The game imports sinf/cosf from the Microsoft UCRT
// (api-ms-win-crt-math-l1-1-0.dll); libm must never be used for these in sim/.
#pragma once

namespace rlios::sim {

// Bit-exact for |x| <= pi/4 (float). Outside that range the result is NOT guaranteed exact.
float crtSinf(float x);
float crtCosf(float x);
bool crtTrigInRange(float x);

// NOT exact yet: the UCRT atan2f (reached only from btClampNormal, when a contact normal is
// clamped at a convex mesh edge) is not ported; this forwards to libm.
float crtAtan2f(float y, float x);

// Number of calls so far that left the proven-exact range (sinf/cosf beyond pi/4, any atan2f).
// Diagnostic only, never read by the sim. The exactness harness reports it, so a scenario that
// passes with a non-zero count is known to have touched an unproven path.
unsigned long long crtInexactCalls();

} // namespace rlios::sim
