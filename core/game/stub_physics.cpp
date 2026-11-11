// PLACEHOLDER physics (see stub_physics.h). Constants here are tuning values for a playable
// stand-in, not game values, except where a source is named.
#include "game/stub_physics.h"

#include "game/hash.h"

namespace rlios::game {

namespace {
using sim::Vec3;

constexpr float kDt = 1.0f / 120.0f;
constexpr float kCarRestZ = 17.0f; // [RS] CAR_SPAWN_REST_Z (cross-check only)
constexpr float kMaxDriveSpeed = 1410.0f;
constexpr float kDriveAccel = 1600.0f;
constexpr float kBoostAccel = 991.666f; // [RS] BOOST_ACCEL_GROUND ~ 2975/3
constexpr float kMaxCarSpeed = 2300.0f; // [ARCH] Car_Default.MaxLinearSpeed = 2300
constexpr float kSupersonic = 2200.0f;  // [RS] SUPERSONIC_START_SPEED, UNPROVEN
constexpr float kTurnRate = 2.5f;       // rad/s at full steer (stand-in)
constexpr float kCoast = 0.995f;
constexpr float kGravity = -650.0f; // [RS] GRAVITY_Z (stand-in; the real ball sim measures its own)
constexpr float kBallR = 91.25f;
constexpr float kBounce = 0.6f;
constexpr float kBallDrag = 0.9995f;
constexpr float kArenaX = 4096.0f;  // [RS] ARENA_EXTENT_X
constexpr float kArenaY = 5120.0f;  // [RS] ARENA_EXTENT_Y
constexpr float kCeiling = 2044.0f; // [RS] ARENA_HEIGHT
constexpr float kGoalHalfW = 892.0f;
constexpr float kGoalH = 642.0f;
constexpr float kGoalBack = 6000.0f;
constexpr float kCarR = 60.0f;       // car "radius" for contacts (stand-in)
constexpr float kHitFrontDot = 0.7f; // forward . (victim - attacker) for a front hit (stand-in)

Vec3 vadd(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 vsub(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 vscale(Vec3 a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
float vdot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
float vlen(Vec3 a) {
    return sim::sqrtF(vdot(a, a));
}

// Forward vector for yaw multiples of 8192 (45 deg); every map start uses one.
Vec3 yawForward(int32_t yawUU) {
    constexpr float h = 0.70710677f;
    static const Vec3 k[8] = {{1, 0, 0},  {h, h, 0},   {0, 1, 0},  {-h, h, 0},
                              {-1, 0, 0}, {-h, -h, 0}, {0, -1, 0}, {h, -h, 0}};
    return k[((yawUU + 4096) & 65535) / 8192];
}
} // namespace

StubPhysics::StubPhysics() : cars_{}, active_{}, ball_{} {}

void StubPhysics::setCarCount(int n) {
    n_ = n;
    for (int i = 0; i < kMaxCars; ++i)
        active_[i] = i < n ? 1 : 0;
}

void StubPhysics::placeCar(int i, const Vec3& pos, int32_t yawUU) {
    CarBody& c = cars_[i];
    c = CarBody{};
    c.pos = {pos.x, pos.y, kCarRestZ};
    c.forward = yawForward(yawUU);
    c.onGround = 1;
}

void StubPhysics::setCarActive(int i, bool active) {
    active_[i] = active ? 1 : 0;
}

void StubPhysics::placeBall(const Vec3& pos) {
    ball_ = BallBody{pos, {0, 0, 0}};
}

void StubPhysics::step(const CarControls* ctl, const uint8_t* boostActive, StepEvents& ev) {
    ev.ballTouchMask = 0;
    ev.numHits = 0;
    // Cars.
    for (int i = 0; i < n_; ++i) {
        if (!active_[i])
            continue;
        CarBody& c = cars_[i];
        float v = vdot(c.vel, c.forward);
        // Turn: small-angle rotation of the forward vector, renormalised.
        const float a = ctl[i].steer * kTurnRate * kDt;
        Vec3 f = {c.forward.x - a * c.forward.y, c.forward.y + a * c.forward.x, 0.0f};
        f = vscale(f, 1.0f / vlen(f));
        c.forward = f;
        float acc = 0.0f;
        const float th = ctl[i].throttle;
        if ((th > 0.0f && v < kMaxDriveSpeed) || (th < 0.0f && v > -kMaxDriveSpeed))
            acc = th * kDriveAccel;
        if (boostActive[i])
            acc = acc + kBoostAccel;
        v = v * kCoast + acc * kDt;
        if (v > kMaxCarSpeed)
            v = kMaxCarSpeed;
        if (v < -kMaxCarSpeed)
            v = -kMaxCarSpeed;
        c.vel = vscale(f, v);
        c.pos = vadd(c.pos, vscale(c.vel, kDt));
        const float lx = kArenaX - kCarR, ly = kArenaY - kCarR;
        if (c.pos.x > lx || c.pos.x < -lx) {
            c.pos.x = c.pos.x > 0.0f ? lx : -lx;
            c.vel = vscale(c.vel, 0.5f);
        }
        if (c.pos.y > ly || c.pos.y < -ly) {
            c.pos.y = c.pos.y > 0.0f ? ly : -ly;
            c.vel = vscale(c.vel, 0.5f);
        }
        c.supersonic = vlen(c.vel) >= kSupersonic ? 1 : 0;
    }
    // Car vs car: front hits are reported; bodies are pushed apart.
    for (int i = 0; i < n_; ++i) {
        for (int j = i + 1; j < n_; ++j) {
            if (!active_[i] || !active_[j])
                continue;
            CarBody& a = cars_[i];
            CarBody& b = cars_[j];
            Vec3 d = vsub(b.pos, a.pos);
            const float dist = vlen(d);
            if (dist >= 2.0f * kCarR || dist <= 0.0f)
                continue;
            const Vec3 n = vscale(d, 1.0f / dist);
            if (vdot(a.forward, n) > kHitFrontDot && ev.numHits < kMaxCars * kMaxCars)
                ev.hits[ev.numHits++] = {i, j};
            if (vdot(b.forward, vscale(n, -1.0f)) > kHitFrontDot &&
                ev.numHits < kMaxCars * kMaxCars)
                ev.hits[ev.numHits++] = {j, i};
            const float push = (2.0f * kCarR - dist) * 0.5f;
            a.pos = vsub(a.pos, vscale(n, push));
            b.pos = vadd(b.pos, vscale(n, push));
            const Vec3 va = a.vel;
            a.vel = vscale(b.vel, 0.5f);
            b.vel = vadd(vscale(va, 0.5f), vscale(n, 300.0f));
        }
    }
    // Ball.
    BallBody& bl = ball_;
    bl.vel.z = bl.vel.z + kGravity * kDt;
    bl.vel = vscale(bl.vel, kBallDrag);
    bl.pos = vadd(bl.pos, vscale(bl.vel, kDt));
    if (bl.pos.z < kBallR) {
        bl.pos.z = kBallR;
        bl.vel.z = bl.vel.z < 0.0f ? -bl.vel.z * kBounce : bl.vel.z;
    }
    if (bl.pos.z > kCeiling - kBallR) {
        bl.pos.z = kCeiling - kBallR;
        bl.vel.z = -bl.vel.z * kBounce;
    }
    if (bl.pos.x > kArenaX - kBallR || bl.pos.x < -(kArenaX - kBallR)) {
        bl.pos.x = bl.pos.x > 0.0f ? kArenaX - kBallR : -(kArenaX - kBallR);
        bl.vel.x = -bl.vel.x * kBounce;
    }
    const bool inMouth = (bl.pos.x < kGoalHalfW - kBallR && bl.pos.x > -(kGoalHalfW - kBallR) &&
                          bl.pos.z < kGoalH - kBallR);
    const float backY = inMouth ? kGoalBack - kBallR : kArenaY - kBallR;
    if (bl.pos.y > backY || bl.pos.y < -backY) {
        bl.pos.y = bl.pos.y > 0.0f ? backY : -backY;
        bl.vel.y = -bl.vel.y * kBounce;
    }
    // Car vs ball.
    for (int i = 0; i < n_; ++i) {
        if (!active_[i])
            continue;
        const CarBody& c = cars_[i];
        const Vec3 hit = {c.pos.x + c.forward.x * 40.0f, c.pos.y + c.forward.y * 40.0f,
                          c.pos.z + 20.0f};
        const Vec3 d = vsub(bl.pos, hit);
        const float dist = vlen(d);
        if (dist >= kBallR + kCarR || dist <= 0.0f)
            continue;
        const Vec3 n = vscale(d, 1.0f / dist);
        const float vrel = vdot(vsub(c.vel, bl.vel), n);
        if (vrel > 0.0f)
            bl.vel = vadd(bl.vel, vscale(n, vrel * 1.5f + 200.0f));
        bl.pos = vadd(hit, vscale(n, kBallR + kCarR));
        ev.ballTouchMask |= 1u << i;
    }
}

uint64_t StubPhysics::hash() const {
    Fnv h;
    for (int i = 0; i < n_; ++i) {
        h.vec(cars_[i].pos);
        h.vec(cars_[i].vel);
        h.vec(cars_[i].forward);
        h.u32(active_[i]);
    }
    h.vec(ball_.pos);
    h.vec(ball_.vel);
    return h.v;
}

} // namespace rlios::game
