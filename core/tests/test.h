// Tiny self-contained test macros (no external deps).
#pragma once

#include <cstdio>
#include <cstdlib>

namespace rlios_test {
inline int& failures() {
    static int n = 0;
    return n;
}
} // namespace rlios_test

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++rlios_test::failures();                                                              \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        const auto va_ = (a);                                                                      \
        const auto vb_ = (b);                                                                      \
        if (!(va_ == vb_)) {                                                                       \
            std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s != %s\n", __FILE__, __LINE__, #a,     \
                         #b);                                                                      \
            ++rlios_test::failures();                                                              \
        }                                                                                          \
    } while (0)

#define TEST_MAIN_RETURN() return rlios_test::failures() ? 1 : (std::puts("OK"), 0)
