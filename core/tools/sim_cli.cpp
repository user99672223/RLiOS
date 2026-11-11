// Headless runner: runs a scenario (or N ticks) and dumps per-tick state as hex bit patterns.
//   rlios_sim_cli [--scenario file] [--ticks N] [--out file]
// Line format: tick x y vx vy hash   (floats as %08x bit patterns, hash as %016llx)
// Final line:  final_hash=<hash>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "game/scenario.h"
#include "sim/state.h"

int main(int argc, char** argv) {
    const char* scenarioPath = nullptr;
    const char* outPath = nullptr;
    long long ticks = -1;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--scenario") && i + 1 < argc) {
            scenarioPath = argv[++i];
        } else if (!std::strcmp(argv[i], "--ticks") && i + 1 < argc) {
            ticks = std::atoll(argv[++i]);
        } else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) {
            outPath = argv[++i];
        } else {
            std::fprintf(stderr, "usage: %s [--scenario f] [--ticks N] [--out f]\n", argv[0]);
            return 2;
        }
    }
    rlios::game::Scenario sc;
    if (scenarioPath) {
        std::ifstream f(scenarioPath);
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", scenarioPath);
            return 2;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        std::string err;
        if (!rlios::game::parseScenario(ss.str(), sc, err)) {
            std::fprintf(stderr, "scenario error: %s\n", err.c_str());
            return 2;
        }
    }
    if (ticks >= 0)
        sc.ticks = static_cast<uint64_t>(ticks);
    if (!scenarioPath && ticks < 0) {
        std::fprintf(stderr, "need --scenario or --ticks\n");
        return 2;
    }

    FILE* out = stdout;
    if (outPath) {
        out = std::fopen(outPath, "wb");
        if (!out) {
            std::fprintf(stderr, "cannot write %s\n", outPath);
            return 2;
        }
    }
    uint64_t last = 0;
    rlios::game::runScenario(sc, [&](const rlios::sim::State& s) {
        last = rlios::sim::hashState(s);
        std::fprintf(out, "%" PRIu64 " %08x %08x %08x %08x %016" PRIx64 "\n", s.tick,
                     rlios::sim::floatBits(s.x), rlios::sim::floatBits(s.y),
                     rlios::sim::floatBits(s.vx), rlios::sim::floatBits(s.vy), last);
    });
    std::fprintf(out, "final_hash=%016" PRIx64 "\n", last);
    if (outPath)
        std::fclose(out);
    return 0;
}
