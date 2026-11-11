#include "sim/rb_state.h"

namespace rlios::sim {

namespace {

// cvtss2si with the default MXCSR rounding mode (round to nearest, ties to even).
// The sim never changes the rounding mode, so nearbyint is the same operation.
int32_t roundNearestEven(float f) {
    return static_cast<int32_t>(std::nearbyint(f));
}

// Shared tail of the game's fixed-point writers: cvtss2si(f + f + 0.5f) >> 1 (arithmetic shift).
int32_t roundHalf(float f) {
    return roundNearestEven(f + f + 0.5f) >> 1;
}

constexpr int kQuatBits = 18;                     // RVA 0xDF6F80 passes 18 to 0x3879A0
constexpr float kQuatMinLen2 = 1.0e-8f;           // RVA 0x1B0F430, bits 0x322BCC77
constexpr float kSqrt2 = 1.41421354f;             // RVA 0x1B12D2C, bits 0x3FB504F3
constexpr float kInvSqrt2 = 0.707106769f;         // RVA 0x1A0E3B0, bits 0x3F3504F3
constexpr Quat kIdentity{0.0f, 0.0f, 0.0f, 1.0f}; // RVA 0x223ABE0

} // namespace

float quantizePos(float uu) {
    // RVA 0x388190 with scale 100.0f: write i = roundHalf(scale * p); read (1/scale) * i.
    const float scale = 100.0f;
    const int32_t i = roundHalf(scale * uu);
    const float inv = 1.0f / scale;
    return inv * static_cast<float>(i);
}

float quantizeVel(float v, float scale) {
    // RVA 0x388260: write i = (int)(scale * v) (cvttss2si, truncation); read f = inv * i, then
    // out = sign(f) * (inv * 0.1f) + f. 0.1f is RVA 0x1A0E390.
    const int32_t i = static_cast<int32_t>(scale * v);
    const float inv = 1.0f / scale;
    const float f = inv * static_cast<float>(i);
    int sign;
    if (f <= 0.0f)
        sign = (0.0f <= f) ? 0 : -1;
    else
        sign = 1;
    const float nudge = inv * 0.1f;
    return static_cast<float>(sign) * nudge + f;
}

Quat quantizeQuat(const Quat& in) {
    // --- write side (RVA 0x3837C0) ---
    const float len2 = in.y * in.y + in.x * in.x + in.z * in.z + in.w * in.w;
    Quat q = kIdentity;
    if (kQuatMinLen2 < len2) {
        const float inv = 1.0f / sqrtF(len2);
        q.w = inv * in.w;
        q.x = inv * in.x;
        q.y = inv * in.y;
        q.z = inv * in.z;
    }
    const float c[4] = {q.x, q.y, q.z, q.w};
    const float a[4] = {std::fabs(q.x), std::fabs(q.y), std::fabs(q.z), std::fabs(q.w)};
    int first = 0;
    if (a[0] < a[1])
        first = 1;
    int second = 2;
    if (a[2] < a[3])
        second = 3;
    int largest = second;
    if (a[second] <= a[first])
        largest = first;

    float rest[3];
    for (int i = 0, n = 0; i < 4; ++i) {
        if (i != largest)
            rest[n++] = c[i];
    }
    // RVA 0x37B1A0: flip so the dropped component is positive, then 18-bit fixed point.
    if (c[largest] < 0.0f) {
        for (float& r : rest)
            r = r * -1.0f;
    }
    const int32_t maxv = (1 << kQuatBits) - 1;
    const float fmax = static_cast<float>(maxv);
    uint32_t packed[3];
    for (int i = 0; i < 3; ++i) {
        const float f = (rest[i] * kSqrt2 * 0.5f + 0.5f) * fmax;
        int32_t v = roundHalf(f);
        if (v < 0)
            v = 0;
        else if (v >= maxv)
            v = maxv;
        packed[i] = static_cast<uint32_t>(v);
    }

    // --- read side (RVA 0x3879A0) ---
    float r[3];
    for (int i = 0; i < 3; ++i) {
        const float t = static_cast<float>(packed[i]) / fmax - 0.5f;
        r[i] = (t + t) * kInvSqrt2;
    }
    // The dropped component is reconstructed through a double sqrt (sqrtpd), then narrowed.
    const double rem = static_cast<double>(((1.0f - r[0] * r[0]) - r[1] * r[1]) - r[2] * r[2]);
    const float big = static_cast<float>(std::sqrt(rem));
    float o[4];
    for (int i = 0, n = 0; i < 4; ++i)
        o[i] = (i == largest) ? big : r[n++];
    const float olen2 = o[0] * o[0] + o[1] * o[1] + o[2] * o[2] + o[3] * o[3];
    if (olen2 <= kQuatMinLen2)
        return kIdentity;
    const float inv = 1.0f / sqrtF(olen2);
    return {o[0] * inv, o[1] * inv, o[2] * inv, o[3] * inv};
}

void quantize(RBState& s) {
    // Field order in the stream (RVA 0xDF6F80): sleeping bit, position (scale 100), rotation,
    // then, only if not sleeping, linear velocity (scale 100) and angular velocity (scale 10000,
    // RVA 0x1B34128). A sleeping state reads back zero velocities.
    s.pos = {quantizePos(s.pos.x), quantizePos(s.pos.y), quantizePos(s.pos.z)};
    s.rot = quantizeQuat(s.rot);
    if (s.sleeping) {
        s.linVel = {0.0f, 0.0f, 0.0f};
        s.angVel = {0.0f, 0.0f, 0.0f};
    } else {
        s.linVel = {quantizeVel(s.linVel.x, 100.0f), quantizeVel(s.linVel.y, 100.0f),
                    quantizeVel(s.linVel.z, 100.0f)};
        s.angVel = {quantizeVel(s.angVel.x, 10000.0f), quantizeVel(s.angVel.y, 10000.0f),
                    quantizeVel(s.angVel.z, 10000.0f)};
    }
}

} // namespace rlios::sim
