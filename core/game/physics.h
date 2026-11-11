// The narrow boundary between the game rules (core/game) and the physics (core/sim).
// The rules only place/remove bodies, pass per-car controls plus the boost decision, step one
// 120 Hz tick, and read back body states and contact events. Units: uu, uu/s.
#pragma once

#include <cstdint>

#include "sim/vec.h"

namespace rlios::game {

inline constexpr int kMaxCars = 8;

struct CarControls {
    float throttle; // -1..1
    float steer;    // -1..1
    float pitch, yaw, roll;
    uint8_t jump;
    uint8_t boost; // boost button held (the rules decide whether boost is actually applied)
    uint8_t handbrake;
    uint8_t pad;
};

struct CarBody {
    sim::Vec3 pos;
    sim::Vec3 vel;
    sim::Vec3 forward; // unit
    uint8_t onGround;
    uint8_t supersonic;
    uint8_t pad[2];
};

struct BallBody {
    sim::Vec3 pos;
    sim::Vec3 vel;
};

// A car hit another car with its front this tick (a bump; may become a demolition).
struct CarHit {
    int32_t attacker;
    int32_t victim;
};

struct StepEvents {
    uint32_t ballTouchMask; // bit i: car i touched the ball this tick
    int32_t numHits;
    CarHit hits[kMaxCars * kMaxCars];
};

class Physics {
  public:
    virtual ~Physics() = default;
    virtual void setCarCount(int n) = 0;
    // Car at rest at `pos`, facing `yawUU` (UE3 rotator units).
    virtual void placeCar(int i, const sim::Vec3& pos, int32_t yawUU) = 0;
    // Inactive cars (demolished) do not move or collide.
    virtual void setCarActive(int i, bool active) = 0;
    virtual void placeBall(const sim::Vec3& pos) = 0;
    // One tick. boostActive[i] != 0: apply boost force to car i this tick.
    virtual void step(const CarControls* controls, const uint8_t* boostActive, StepEvents& ev) = 0;
    virtual CarBody car(int i) const = 0;
    virtual BallBody ball() const = 0;
    virtual uint64_t hash() const = 0;
};

} // namespace rlios::game
