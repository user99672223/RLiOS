// Golden test: whole bot-vs-bot matches on the placeholder physics must reproduce these final
// state hashes bit for bit, on every platform (x86-64 and arm64 in CI). Update the table only
// for an intended rules/placeholder change, and say so in the commit message.
#include <cstdio>

#include "game/bot_match.h"
#include "test.h"

using namespace rlios::game;

namespace {
struct Golden {
    int32_t blue, orange;
    uint64_t seed;
    uint64_t hash;
};
const Golden kGolden[] = {
    {1, 1, 1, 0xa022c81554806a4bull},
    {2, 2, 2, 0xbd85eafa5c78c580ull},
    {3, 3, 3, 0x19dd762644c948f4ull},
};
} // namespace

int main() {
    for (const Golden& g : kGolden) {
        MatchConfig cfg;
        cfg.blueCars = g.blue;
        cfg.orangeCars = g.orange;
        cfg.seed = g.seed;
        BotMatch a(cfg);
        BotMatch b(cfg);
        a.step(120u * 60u * 30u);
        b.step(120u * 60u * 30u);
        const MatchState& s = a.match().state();
        std::printf("%dv%d seed %llu: tick %llu score %d-%d ot %d hash 0x%016llxull\n", g.blue,
                    g.orange, static_cast<unsigned long long>(g.seed),
                    static_cast<unsigned long long>(s.tick), s.score[0], s.score[1], s.overtime,
                    static_cast<unsigned long long>(a.match().hash()));
        CHECK(a.ended());
        CHECK_EQ(a.match().hash(), b.match().hash());
        CHECK(s.score[0] + s.score[1] > 0);
        CHECK_EQ(a.match().hash(), g.hash);
    }
    TEST_MAIN_RETURN();
}
