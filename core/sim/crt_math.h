// The transcendental functions the physics path calls, reproduced so every platform returns the
// bits the Windows game computes. The game imports sinf/cosf from the Microsoft UCRT
// (api-ms-win-crt-math-l1-1-0.dll); libm must never be used for these in sim/.
#pragma once

namespace rlios::sim {

float crtSinf(float x);
float crtCosf(float x);

} // namespace rlios::sim
