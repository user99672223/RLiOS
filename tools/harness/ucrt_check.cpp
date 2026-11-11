// Laptop-only check of core/sim/crt_math.cpp against the real Microsoft UCRT the game ships.
// Windows program (built with mingw-w64, run under Wine): loads the given ucrtbase.dll, and
// compares sinf/cosf with our port for EVERY float in [-pi/4, pi/4], by bit pattern. Also prints
// powf(1 - 0.03f, 1/120) (the ball's per-tick linear damping factor used as a constant in sim/).
//
// Build + run (see tools/harness/ucrt_check.sh):
//   x86_64-w64-mingw32-g++ -O2 -mfma -ffp-contract=off -static -Icore ucrt_check.cpp
//       core/sim/crt_math.cpp -o ucrt_check.exe
//   wine ucrt_check.exe 'Z:\path\to\game\Binaries\Win64\ucrtbase.dll'
// The DLL is never copied or committed.
#include "sim/crt_math.h"
#include "sim/vec.h"

#include <cstdio>
#include <windows.h>

using namespace rlios::sim;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ucrt_check <ucrtbase.dll>\n");
        return 2;
    }
    HMODULE h = LoadLibraryA(argv[1]);
    if (!h) {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 2;
    }
    typedef float (*fn1)(float);
    typedef float (*fn2)(float, float);
    fn1 msSin = reinterpret_cast<fn1>(reinterpret_cast<void*>(GetProcAddress(h, "sinf")));
    fn1 msCos = reinterpret_cast<fn1>(reinterpret_cast<void*>(GetProcAddress(h, "cosf")));
    fn2 msPow = reinterpret_cast<fn2>(reinterpret_cast<void*>(GetProcAddress(h, "powf")));
    if (!msSin || !msCos || !msPow)
        return 2;
    // CPU-feature flag that selects the FMA3 path (RVA 0x138E60 in 10.0.26100.2454).
    std::printf("fma flag: %d\n", *reinterpret_cast<int*>(reinterpret_cast<char*>(h) + 0x138E60));
    unsigned long long bad = 0, n = 0;
    for (uint32_t sign = 0; sign < 2; ++sign) {
        for (uint32_t b = 0; b <= 0x3F490FDBu; ++b) {
            const float x = bitsFloat(b | (sign << 31));
            const uint32_t s0 = floatBits(msSin(x)), s1 = floatBits(crtSinf(x));
            const uint32_t c0 = floatBits(msCos(x)), c1 = floatBits(crtCosf(x));
            ++n;
            if (s0 != s1 || c0 != c1) {
                if (bad < 10)
                    std::printf("x %08X sin ms %08X ours %08X cos ms %08X ours %08X\n",
                                floatBits(x), s0, s1, c0, c1);
                ++bad;
            }
        }
    }
    std::printf("sinf/cosf: %llu inputs, %llu mismatches\n", n, bad);
    const float damp = msPow(1.0f - 0.03f, bitsFloat(0x3C088889u));
    std::printf("powf(1-0.03f, dt) = %08X, powf(1-0, dt) = %08X\n", floatBits(damp),
                floatBits(msPow(1.0f - 0.0f, bitsFloat(0x3C088889u))));
    return bad ? 1 : 0;
}
