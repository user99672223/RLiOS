#include "sim/crt_math.h"

#include <cmath>

namespace rlios::sim {

// PROVISIONAL: double-precision libm narrowed to float. Exact for the zero-rotation path
// (sin(0) = 0, cos(0) = 1); to be replaced by a port of the UCRT routines.
float crtSinf(float x) {
    return static_cast<float>(std::sin(static_cast<double>(x)));
}

float crtCosf(float x) {
    return static_cast<float>(std::cos(static_cast<double>(x)));
}

} // namespace rlios::sim
