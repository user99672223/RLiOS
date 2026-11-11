// PLACEHOLDER physics for the rules layer. This is NOT Rocket League physics: flat-floor cars with
// full grip, a bouncing ball and crude contacts, enough to play a whole match with bots. It is
// replaced by the real ball/car simulation (core/sim, owned by the sim workers) behind the same
// game::Physics interface. Only + - * / and sqrt, so it is deterministic across x86-64 and arm64.
#pragma once

#include "game/physics.h"

namespace rlios::game {

class StubPhysics final : public Physics {
  public:
    StubPhysics();
    void setCarCount(int n) override;
    void placeCar(int i, const sim::Vec3& pos, int32_t yawUU) override;
    void setCarActive(int i, bool active) override;
    void placeBall(const sim::Vec3& pos) override;
    void step(const CarControls* controls, const uint8_t* boostActive, StepEvents& ev) override;
    CarBody car(int i) const override { return cars_[i]; }
    BallBody ball() const override { return ball_; }
    uint64_t hash() const override;

  private:
    int n_ = 0;
    CarBody cars_[kMaxCars];
    uint8_t active_[kMaxCars];
    BallBody ball_;
};

} // namespace rlios::game
