#include <atomic>
#include <cstdint>
#include <thread>

#include "sim/triple_buffer.h"
#include "test.h"

using rlios::sim::TripleBuffer;

namespace {
// Fields are redundant so a torn read (partially updated buffer) is detectable.
struct Payload {
    uint64_t seq;
    uint64_t seqCopy;
    uint64_t seqInv;
    uint64_t pad[5];
};
} // namespace

int main() {
    {
        TripleBuffer<Payload> tb;
        CHECK(!tb.acquire()); // nothing published yet
        tb.back() = Payload{1, 1, ~1ull, {}};
        tb.publish();
        CHECK(tb.acquire());
        CHECK_EQ(tb.front().seq, 1u);
        CHECK(!tb.acquire()); // no newer data
        // Latest wins: publish 2 then 3, consumer must see 3.
        tb.back() = Payload{2, 2, ~2ull, {}};
        tb.publish();
        tb.back() = Payload{3, 3, ~3ull, {}};
        tb.publish();
        CHECK(tb.acquire());
        CHECK_EQ(tb.front().seq, 3u);
    }
    // Concurrent stress: producer publishes monotonically increasing sequences.
    {
        TripleBuffer<Payload> tb;
        std::atomic<bool> done{false};
        constexpr uint64_t kN = 2000000;
        std::thread prod([&] {
            for (uint64_t i = 1; i <= kN; ++i) {
                Payload& p = tb.back();
                p.seq = i;
                p.seqCopy = i;
                p.seqInv = ~i;
                tb.publish();
            }
            done.store(true);
        });
        uint64_t last = 0, seen = 0;
        bool torn = false, backwards = false;
        while (true) {
            const bool fin = done.load();
            if (tb.acquire()) {
                const Payload& p = tb.front();
                if (p.seq != p.seqCopy || p.seqInv != ~p.seq)
                    torn = true;
                if (p.seq < last)
                    backwards = true;
                last = p.seq;
                ++seen;
            }
            if (fin) {
                if (!tb.acquire())
                    break;
                const Payload& p = tb.front();
                last = p.seq;
                ++seen;
            }
        }
        prod.join();
        CHECK(!torn);
        CHECK(!backwards);
        CHECK_EQ(last, kN);
        CHECK(seen > 0);
    }
    TEST_MAIN_RETURN();
}
