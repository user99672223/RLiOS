// Headless bot-vs-bot soccar match on the placeholder physics.
//   rlios_match_cli [--blue N] [--orange N] [--seed S] [--max-ticks T] [--trace every]
// Prints goals/demos/end state and the final match hash; exit 1 on a non-finite state or if the
// match does not end within --max-ticks.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/bot_match.h"

using namespace rlios::game;

int main(int argc, char** argv) {
    MatchConfig cfg;
    cfg.blueCars = 3;
    cfg.orangeCars = 3;
    cfg.seed = 1;
    uint64_t maxTicks = 120ull * 60 * 30;
    uint64_t trace = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--blue"))
            cfg.blueCars = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--orange"))
            cfg.orangeCars = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--seed"))
            cfg.seed = std::strtoull(argv[i + 1], nullptr, 0);
        else if (!std::strcmp(argv[i], "--max-ticks"))
            maxTicks = std::strtoull(argv[i + 1], nullptr, 0);
        else if (!std::strcmp(argv[i], "--trace"))
            trace = std::strtoull(argv[i + 1], nullptr, 0);
    }
    BotMatch bm(cfg);
    int32_t goals = 0;
    int32_t demos = 0;
    while (!bm.ended() && bm.match().state().tick < maxTicks) {
        bm.step(1);
        const MatchState& s = bm.match().state();
        const BallBody b = bm.physics().ball();
        if (!std::isfinite(b.pos.x) || !std::isfinite(b.pos.y) || !std::isfinite(b.pos.z)) {
            std::printf("non-finite ball at tick %llu\n", static_cast<unsigned long long>(s.tick));
            return 1;
        }
        if (s.goals != goals) {
            goals = s.goals;
            std::printf("tick %llu goal: %d-%d clock %d%s\n",
                        static_cast<unsigned long long>(s.tick), s.score[0], s.score[1],
                        bm.match().displaySeconds(), s.overtime ? " OT" : "");
        }
        int32_t d = 0;
        for (int i = 0; i < s.numCars; ++i)
            d += s.players[i].demos;
        if (d != demos) {
            demos = d;
            std::printf("tick %llu demolition (total %d)\n",
                        static_cast<unsigned long long>(s.tick), d);
        }
        if (trace && s.tick % trace == 0)
            std::printf("tick %llu phase %d clock %d ball %.1f %.1f %.1f hash %016llx\n",
                        static_cast<unsigned long long>(s.tick), static_cast<int>(s.phase),
                        bm.match().displaySeconds(), static_cast<double>(b.pos.x),
                        static_cast<double>(b.pos.y), static_cast<double>(b.pos.z),
                        static_cast<unsigned long long>(bm.match().hash()));
    }
    const MatchState& s = bm.match().state();
    std::printf(
        "end tick %llu phase %d score %d-%d overtime %d (%d ticks) winner %d hash %016llx\n",
        static_cast<unsigned long long>(s.tick), static_cast<int>(s.phase), s.score[0], s.score[1],
        s.overtime, s.overtimeTicks, s.winner, static_cast<unsigned long long>(bm.match().hash()));
    return bm.ended() ? 0 : 1;
}
