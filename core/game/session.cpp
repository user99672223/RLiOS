#include "game/session.h"

#include <algorithm>
#include <chrono>

namespace rlios::game {

namespace {
uint64_t packInput(float ax, float ay) {
    return (static_cast<uint64_t>(sim::floatBits(ay)) << 32) | sim::floatBits(ax);
}
} // namespace

uint64_t monotonicNowNs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

Session::Session(const SessionConfig& cfg)
    : cfg_(cfg), state_(sim::initialState()), ts_(sim::kTickHz, cfg.maxCatchUp),
      input_(packInput(0.0f, 0.0f)) {
    // Publish tick 0 so consumers always have a valid snapshot.
    Snapshot& s = buffer_.back();
    s.valid = 1;
    s.pad = 0;
    s.prev = state_;
    s.curr = state_;
    s.hash = sim::hashState(state_);
    s.currTimeNs = 0;
    buffer_.publish();
    hash_.store(s.hash);
}

Session::~Session() {
    stop();
}

void Session::setInput(float ax, float ay) {
    input_.store(packInput(ax, ay));
}

void Session::tickOnce() {
    const uint64_t in = input_.load(std::memory_order_relaxed);
    const sim::Input input{sim::bitsFloat(static_cast<uint32_t>(in & 0xffffffffu)),
                           sim::bitsFloat(static_cast<uint32_t>(in >> 32))};
    Snapshot& s = buffer_.back();
    s.valid = 1;
    s.pad = 0;
    s.prev = state_;
    sim::step(state_, input);
    s.curr = state_;
    s.hash = sim::hashState(state_);
    s.currTimeNs = cfg_.manual ? 0 : startNs_ + sim::tickTimeNs(state_.tick);
    buffer_.publish();
    tick_.store(state_.tick, std::memory_order_relaxed);
    hash_.store(s.hash, std::memory_order_relaxed);
}

void Session::step(uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        tickOnce();
}

bool Session::start() {
    if (cfg_.manual || thread_.joinable())
        return false;
    stopFlag_.store(false);
    startNs_ = monotonicNowNs() - sim::tickTimeNs(state_.tick);
    thread_ = std::thread([this] { threadMain(); });
    return true;
}

void Session::stop() {
    if (!thread_.joinable())
        return;
    stopFlag_.store(true);
    thread_.join();
    running_.store(0);
}

void Session::threadMain() {
    running_.store(1);
    uint64_t last = monotonicNowNs();
    uint64_t rateT0 = last;
    uint64_t rateTick0 = state_.tick;
    while (!stopFlag_.load(std::memory_order_relaxed)) {
        const uint64_t now = monotonicNowNs();
        const uint32_t n = ts_.advance(now - last);
        last = now;
        for (uint32_t i = 0; i < n; ++i)
            tickOnce();
        dropped_.store(ts_.droppedTicks(), std::memory_order_relaxed);
        if (now - rateT0 >= sim::FixedTimestep::kNsPerSec) {
            const double secs = static_cast<double>(now - rateT0) * 1e-9;
            rate_.store(static_cast<double>(state_.tick - rateTick0) / secs);
            rateT0 = now;
            rateTick0 = state_.tick;
        }
        // Sleep until the next tick is due (capped so stop() stays responsive).
        const uint64_t wait = std::min<uint64_t>(ts_.nsUntilNextTick(), 2000000ull);
        std::this_thread::sleep_for(std::chrono::nanoseconds(wait));
    }
}

bool Session::renderState(uint64_t nowNs, RenderState& out) {
    buffer_.acquire();
    const Snapshot& s = buffer_.front();
    if (!s.valid)
        return false;
    double alpha = 1.0;
    if (!cfg_.manual) {
        // Render one tick behind: display time maps to [prev, curr].
        const double period = 1e9 / static_cast<double>(sim::kTickHz);
        const double a = (static_cast<double>(nowNs) - static_cast<double>(s.currTimeNs)) / period;
        alpha = std::clamp(a, 0.0, 1.0);
    }
    const float a = static_cast<float>(alpha);
    out.tick = s.curr.tick;
    out.alpha = alpha;
    out.x = s.prev.x + (s.curr.x - s.prev.x) * a;
    out.y = s.prev.y + (s.curr.y - s.prev.y) * a;
    out.vx = s.curr.vx;
    out.vy = s.curr.vy;
    out.hash = s.hash;
    return true;
}

Diagnostics Session::diagnostics() const {
    Diagnostics d{};
    d.tick = tick_.load(std::memory_order_relaxed);
    d.hash = hash_.load(std::memory_order_relaxed);
    d.droppedTicks = dropped_.load(std::memory_order_relaxed);
    d.tickRateHz = rate_.load(std::memory_order_relaxed);
    d.running = running_.load(std::memory_order_relaxed);
    return d;
}

} // namespace rlios::game
