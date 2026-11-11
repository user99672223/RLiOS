#include "game/bot.h"

namespace rlios::game {

uint32_t BotInputs::next() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 7;
    rng_ ^= rng_ << 17;
    return static_cast<uint32_t>(rng_ >> 32);
}

void BotInputs::compute(const Match& m, Physics& phys, CarControls* controls) {
    const MatchState& s = m.state();
    const BallBody ball = phys.ball();
    // New jitter every half second per car.
    if (s.tick % 60 == 0) {
        for (int i = 0; i < s.numCars; ++i)
            jitter_[i] = static_cast<float>(next() & 0xffu) * (1.0f / 255.0f) - 0.5f;
    }
    for (int i = 0; i < s.numCars; ++i) {
        CarControls& c = controls[i];
        c = CarControls{};
        const CarBody car = phys.car(i);
        const float goalY = s.players[i].team == 0 ? 5120.0f : -5120.0f;
        // Aim point: 150 uu behind the ball, away from the opponent goal.
        float gx = ball.pos.x;
        float gy = ball.pos.y - goalY;
        const float gl = sim::sqrtF(gx * gx + gy * gy);
        float tx = ball.pos.x;
        float ty = ball.pos.y;
        if (gl > 1.0f) {
            tx = ball.pos.x + gx / gl * 150.0f;
            ty = ball.pos.y + gy / gl * 150.0f;
        }
        tx = tx + jitter_[i] * 200.0f;
        const float dx = tx - car.pos.x;
        const float dy = ty - car.pos.y;
        const float dl = sim::sqrtF(dx * dx + dy * dy);
        if (dl < 1.0f) {
            c.throttle = 1.0f;
            continue;
        }
        const float cross = car.forward.x * dy - car.forward.y * dx;
        const float dotp = (car.forward.x * dx + car.forward.y * dy) / dl;
        float steer = cross / dl * 4.0f;
        if (dotp < 0.0f)
            steer = cross < 0.0f ? -1.0f : 1.0f;
        if (steer > 1.0f)
            steer = 1.0f;
        if (steer < -1.0f)
            steer = -1.0f;
        c.steer = steer;
        c.throttle = 1.0f;
        c.boost = (dotp > 0.9f && dl > 1200.0f) ? 1 : 0;
    }
}

} // namespace rlios::game
