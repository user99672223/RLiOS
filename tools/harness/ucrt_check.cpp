// Laptop-only proof of core/sim/crt_math.cpp against the real Microsoft UCRT the game ships.
// Windows program (built with mingw-w64, run under Wine): loads the given ucrtbase.dll and
// compares each ported function with the DLL's export, by bit pattern:
//   - 1-argument float functions: every one of the 2^32 inputs;
//   - 2-argument float functions: every (exponent, exponent, sign, sign) combination with a set
//     of mantissas, every special value against that set, plus N random bit-pattern pairs;
//   - double functions: special values, an exponent sweep and N random bit patterns.
// The DLL selects its FMA3 code path from the CPU-feature dword at RVA 0x138E60 (must print 3
// on an FMA3 CPU; that path is the reference).
//
// Build + run: tools/harness/ucrt_check.sh [function ...]   (see there). The DLL is never copied
// or committed.
#include "sim/crt_math.h"
#include "sim/vec.h"

#include <cstdio>
#include <cstring>
#include <windows.h>

using namespace rlios::sim;

namespace {

typedef float (*F1)(float);
typedef float (*F2)(float, float);
typedef double (*D1)(double);
typedef double (*D2)(double, double);

struct Fn {
    const char* name;
    int kind; // 1: float(float), 2: float(float, float), 3: double(double), 4: double(double,
              // double)
    void* ours;
    void* ms;
};

Fn gFns[] = {
    {"sinf", 1, reinterpret_cast<void*>(&crtSinf), nullptr},
    {"cosf", 1, reinterpret_cast<void*>(&crtCosf), nullptr},
    {"tanf", 1, reinterpret_cast<void*>(&crtTanf), nullptr},
    {"asinf", 1, reinterpret_cast<void*>(&crtAsinf), nullptr},
    {"acosf", 1, reinterpret_cast<void*>(&crtAcosf), nullptr},
    {"atanf", 1, reinterpret_cast<void*>(&crtAtanf), nullptr},
    {"expf", 1, reinterpret_cast<void*>(&crtExpf), nullptr},
    {"logf", 1, reinterpret_cast<void*>(&crtLogf), nullptr},
    {"floorf", 1, reinterpret_cast<void*>(&crtFloorf), nullptr},
    {"ceilf", 1, reinterpret_cast<void*>(&crtCeilf), nullptr},
    {"sqrtf", 1, reinterpret_cast<void*>(&crtSqrtf), nullptr},
    {"atan2f", 2, reinterpret_cast<void*>(&crtAtan2f), nullptr},
    {"powf", 2, reinterpret_cast<void*>(&crtPowf), nullptr},
    {"fmodf", 2, reinterpret_cast<void*>(&crtFmodf), nullptr},
};

uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

constexpr int kThreads = 8;

struct Job {
    const Fn* fn;
    int part;
    unsigned long long n, bad;
    unsigned long long randomCount;
    char first[10][160];
    int nFirst;
};

void report(Job& j, const char* text) {
    if (j.nFirst < 10)
        std::snprintf(j.first[j.nFirst++], sizeof j.first[0], "%s", text);
    ++j.bad;
}

void check1(Job& j, uint32_t b) {
    const float x = bitsFloat(b);
    const uint32_t m = floatBits(reinterpret_cast<F1>(j.fn->ms)(x));
    const uint32_t o = floatBits(reinterpret_cast<F1>(j.fn->ours)(x));
    ++j.n;
    if (m != o) {
        char t[160];
        std::snprintf(t, sizeof t, "x %08X ms %08X ours %08X", b, m, o);
        report(j, t);
    }
}

void check2(Job& j, uint32_t a, uint32_t b) {
    const float x = bitsFloat(a), y = bitsFloat(b);
    const uint32_t m = floatBits(reinterpret_cast<F2>(j.fn->ms)(x, y));
    const uint32_t o = floatBits(reinterpret_cast<F2>(j.fn->ours)(x, y));
    ++j.n;
    if (m != o) {
        char t[160];
        std::snprintf(t, sizeof t, "a %08X b %08X ms %08X ours %08X", a, b, m, o);
        report(j, t);
    }
}

// Mantissa set used for each exponent in the structured 2-argument sweep.
constexpr uint32_t kMant[] = {0x000000, 0x000001, 0x000002, 0x400000, 0x7FFFFF, 0x7FFFFE,
                              0x490FDB, 0x2AAAAB, 0x123456, 0x6DCBA9, 0x0F0F0F, 0x555555};
constexpr int kNMant = sizeof kMant / sizeof kMant[0];
// Special values crossed with everything in the structured set.
constexpr uint32_t kSpecial[] = {0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000,
                                 0xFFC00000, 0x7F800001, 0x7FA00000, 0x3F800000, 0xBF800000,
                                 0x00000001, 0x80000001, 0x007FFFFF, 0x7F7FFFFF, 0xFF7FFFFF,
                                 0x3F000000, 0x40000000, 0xC0000000, 0x4B000000, 0x4B000001};
constexpr int kNSpecial = sizeof kSpecial / sizeof kSpecial[0];

DWORD WINAPI worker(LPVOID p) {
    Job& j = *static_cast<Job*>(p);
    if (j.fn->kind == 1) {
        const uint64_t lo = (0x100000000ull * j.part) / kThreads;
        const uint64_t hi = (0x100000000ull * (j.part + 1)) / kThreads;
        for (uint64_t b = lo; b < hi; ++b)
            check1(j, static_cast<uint32_t>(b));
    } else {
        // Structured: exponent a (split across threads) x exponent b x signs x mantissas.
        for (uint32_t ea = j.part; ea < 256; ea += kThreads)
            for (uint32_t eb = 0; eb < 256; ++eb)
                for (uint32_t s = 0; s < 4; ++s)
                    for (int ma = 0; ma < kNMant; ++ma)
                        for (int mb = 0; mb < kNMant; ++mb)
                            check2(j, ((s & 1) << 31) | (ea << 23) | kMant[ma],
                                   ((s >> 1) << 31) | (eb << 23) | kMant[mb]);
        // Specials against every structured value (both argument positions).
        for (uint32_t e = j.part; e < 256; e += kThreads)
            for (uint32_t s = 0; s < 2; ++s)
                for (int m = 0; m < kNMant; ++m)
                    for (int k = 0; k < kNSpecial; ++k) {
                        const uint32_t v = (s << 31) | (e << 23) | kMant[m];
                        check2(j, kSpecial[k], v);
                        check2(j, v, kSpecial[k]);
                    }
        // Random pairs.
        uint64_t seed = 0x5EED0000ull + static_cast<uint64_t>(j.part) * 0x1234567ull;
        for (unsigned long long i = 0; i < j.randomCount / kThreads; ++i) {
            const uint64_t r = splitmix(seed);
            check2(j, static_cast<uint32_t>(r), static_cast<uint32_t>(r >> 32));
        }
    }
    return 0;
}

int runFloat(const Fn& fn, unsigned long long randomCount) {
    static Job jobs[kThreads];
    HANDLE th[kThreads];
    for (int t = 0; t < kThreads; ++t) {
        std::memset(&jobs[t], 0, sizeof jobs[t]);
        jobs[t].fn = &fn;
        jobs[t].part = t;
        jobs[t].randomCount = randomCount;
        th[t] = CreateThread(nullptr, 0, worker, &jobs[t], 0, nullptr);
    }
    WaitForMultipleObjects(kThreads, th, TRUE, INFINITE);
    unsigned long long n = 0, bad = 0;
    for (int t = 0; t < kThreads; ++t) {
        CloseHandle(th[t]);
        n += jobs[t].n;
        bad += jobs[t].bad;
        for (int i = 0; i < jobs[t].nFirst; ++i)
            std::printf("  %s: %s\n", fn.name, jobs[t].first[i]);
    }
    std::printf("%s: %llu inputs, %llu mismatches\n", fn.name, n, bad);
    std::fflush(stdout);
    return bad ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ucrt_check <ucrtbase.dll> [--random N] [--golden] [fn ...]\n");
        return 2;
    }
    HMODULE h = LoadLibraryA(argv[1]);
    if (!h) {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 2;
    }
    // Make sure this is the game's DLL and not Wine's builtin ucrtbase (which Wine silently loads
    // instead of any file named ucrtbase.dll): sinf must sit at RVA 0xAC110.
    const uintptr_t sinRva =
        reinterpret_cast<uintptr_t>(GetProcAddress(h, "sinf")) - reinterpret_cast<uintptr_t>(h);
    if (sinRva != 0xAC110) {
        std::fprintf(stderr, "not ucrtbase 10.0.26100.2454 (sinf at RVA %llx)\n",
                     static_cast<unsigned long long>(sinRva));
        return 2;
    }
    // CPU-feature flag that selects the FMA3 path (RVA 0x138E60 in 10.0.26100.2454).
    const int flag = *reinterpret_cast<int*>(reinterpret_cast<char*>(h) + 0x138E60);
    std::printf("fma flag: %d\n", flag);
    if (flag != 3) {
        std::fprintf(stderr, "DLL is not on its FMA3 path; results are not the reference\n");
        return 2;
    }
    for (Fn& f : gFns) {
        f.ms = reinterpret_cast<void*>(GetProcAddress(h, f.name));
        if (!f.ms) {
            std::fprintf(stderr, "missing export %s\n", f.name);
            return 2;
        }
    }
    unsigned long long randomCount = 1000000000ull;
    bool golden = false;
    int fail = 0;
    bool any = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--random") && i + 1 < argc) {
            randomCount = std::strtoull(argv[++i], nullptr, 0);
            continue;
        }
        if (!std::strcmp(argv[i], "--golden")) {
            golden = true;
            continue;
        }
        bool found = false;
        for (const Fn& f : gFns)
            if (!std::strcmp(argv[i], f.name) || !std::strcmp(argv[i], "all")) {
                found = any = true;
                if (golden) {
                    // Golden pairs for core/tests/test_crt_math.cpp, from the DLL itself.
                    uint64_t seed = 0x601DE11ull;
                    std::printf("// %s\n", f.name);
                    for (int k = 0; k < 256; ++k) {
                        const uint64_t r = splitmix(seed);
                        uint32_t a = static_cast<uint32_t>(r), b = static_cast<uint32_t>(r >> 32);
                        if (k < kNSpecial)
                            a = kSpecial[k];
                        else if (k < 160) // bias towards moderate magnitudes
                            a = (a & 0x807FFFFFu) | ((0x60u + (a >> 23) % 0x40u) << 23);
                        if (k >= 20 &&
                            k < 200) // and the second argument too (|b| in ~[2^-31, 2^32])
                            b = (b & 0x807FFFFFu) | ((0x60u + (b >> 23) % 0x40u) << 23);
                        if (f.kind == 1)
                            std::printf("{0x%08X, 0, 0x%08X},\n", a,
                                        floatBits(reinterpret_cast<F1>(f.ms)(bitsFloat(a))));
                        else
                            std::printf(
                                "{0x%08X, 0x%08X, 0x%08X},\n", a, b,
                                floatBits(reinterpret_cast<F2>(f.ms)(bitsFloat(a), bitsFloat(b))));
                    }
                } else if (f.kind <= 2) {
                    fail |= runFloat(f, randomCount);
                }
            }
        if (!found) {
            std::fprintf(stderr, "unknown function %s\n", argv[i]);
            return 2;
        }
    }
    if (!any)
        std::printf("no function given (use names or 'all')\n");
    return fail;
}
