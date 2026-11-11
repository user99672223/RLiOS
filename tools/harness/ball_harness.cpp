// Exactness harness for the ball: replays a recorded scenario through sim/ and compares every
// tick against the real game's recording by float BIT PATTERN (never with an epsilon).
//
// Usage: ball_harness <scenario-dir> [--trace N] [--collision <file.rlcol>]
//   --collision: the arena's triangle meshes (tools/hook/dump_collision.py; game data, never
//   committed). Without it the world has only the built-in planes, and a scenario that says
//   "needs collision" is skipped.
//   <scenario-dir>/scenario.txt  initial state (format: tools/hook/record.py)
//   <scenario-dir>/rec.tsv       recording: hook tick, frame, label, raw bytes as hex
// Row k of the recording is the state after tick k+1 from the scenario's initial state.
// Exit code: 0 all ticks identical, 1 divergence, 2 bad input, 77 reference data not present.
#include "io/arena_collision.h"
#include "sim/ball_world.h"
#include "sim/crt_math.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace rlios::sim;

namespace {

struct Row {
    uint32_t xf[16];
    uint32_t vel[8];
    bool haveXf = false;
    bool haveVel = false;
    bool haveInertia = false;
    uint32_t inertia[12];
};

bool parseHexFloats(const std::string& hex, uint32_t* out, size_t n) {
    if (hex.size() < n * 8)
        return false;
    for (size_t i = 0; i < n; ++i) {
        uint32_t v = 0;
        for (int b = 0; b < 4; ++b) {
            const std::string byte = hex.substr(i * 8 + static_cast<size_t>(b) * 2, 2);
            v |= static_cast<uint32_t>(std::strtoul(byte.c_str(), nullptr, 16)) << (8 * b);
        }
        out[i] = v;
    }
    return true;
}

// A scenario number is a decimal float or exact bits written as 0x41200000.
float parseNumber(const std::string& tok) {
    if (tok.rfind("0x", 0) == 0)
        return bitsFloat(static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 16)));
    return std::strtof(tok.c_str(), nullptr);
}

struct Field {
    const char* name;
    uint32_t ours;
    uint32_t ref;
};

int totalContacts(const BallWorld& w) {
    int n = 0;
    for (const Manifold& m : w.manifolds)
        n += m.count;
    return n;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: ball_harness <scenario-dir> [--trace N] [--collision <file>]\n");
        return 2;
    }
    const std::string dir = argv[1];
    long trace = 0;
    std::string collisionPath;
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--trace") == 0)
            trace = std::strtol(argv[i + 1], nullptr, 10);
        else if (std::strcmp(argv[i], "--collision") == 0)
            collisionPath = argv[i + 1];
    }

    std::ifstream scen(dir + "/scenario.txt");
    std::ifstream rec(dir + "/rec.tsv");
    if (!scen || !rec) {
        std::printf("SKIP: no reference data in %s (recordings are never committed)\n",
                    dir.c_str());
        return 77;
    }

    // Recording.
    std::vector<Row> rows;
    {
        std::string line;
        std::string lastTick;
        while (std::getline(rec, line)) {
            std::istringstream ls(line);
            std::string tick, frame, label, hex;
            if (!std::getline(ls, tick, '\t') || !std::getline(ls, frame, '\t') ||
                !std::getline(ls, label, '\t') || !std::getline(ls, hex, '\t'))
                continue;
            if (rows.empty() || tick != lastTick) {
                rows.emplace_back();
                lastTick = tick;
            }
            Row& r = rows.back();
            if (label == "ball_bt_xform")
                r.haveXf = parseHexFloats(hex, r.xf, 16);
            else if (label == "ball_bt_vel")
                r.haveVel = parseHexFloats(hex, r.vel, 8);
            else if (label == "ball_bt_inertia")
                r.haveInertia = parseHexFloats(hex, r.inertia, 12);
        }
    }
    if (rows.empty() || !rows[0].haveXf || !rows[0].haveVel) {
        std::fprintf(stderr, "empty or malformed recording\n");
        return 2;
    }

    // Initial state. The rotation basis (and with it the world inertia tensor) is whatever the
    // game had unless the scenario sets it; in that case it is taken from the first recorded row,
    // which is only valid for a ball that does not rotate during the first tick.
    Transform xf;
    for (int i = 0; i < 3; ++i)
        xf.basis.r[i] = {bitsFloat(rows[0].xf[i * 4]), bitsFloat(rows[0].xf[i * 4 + 1]),
                         bitsFloat(rows[0].xf[i * 4 + 2])};
    xf.origin = {0.0f, 0.0f, 0.0f};
    Vec3 linVel = {0.0f, 0.0f, 0.0f};
    Vec3 angVel = {0.0f, 0.0f, 0.0f};
    Mat3 inertia{};
    bool haveInertia = false;
    bool havePos = false, haveVel = false, haveAng = false;
    std::string name = dir;
    bool needsCollision = false;
    {
        std::string line;
        while (std::getline(scen, line)) {
            const size_t hash = line.find('#');
            if (hash != std::string::npos)
                line.resize(hash);
            std::istringstream ls(line);
            std::vector<std::string> t;
            for (std::string tok; ls >> tok;)
                t.push_back(tok);
            if (t.size() >= 2 && t[0] == "name")
                name = t[1];
            if (t.size() >= 2 && t[0] == "needs" && t[1] == "collision")
                needsCollision = true;
            if (t.size() < 5 || t[0] != "ball")
                continue;
            if (t[1] == "pos") {
                xf.origin = {parseNumber(t[2]), parseNumber(t[3]), parseNumber(t[4])};
                havePos = true;
            } else if (t[1] == "vel") {
                linVel = {parseNumber(t[2]), parseNumber(t[3]), parseNumber(t[4])};
                haveVel = true;
            } else if (t[1] == "angvel") {
                angVel = {parseNumber(t[2]), parseNumber(t[3]), parseNumber(t[4])};
                haveAng = true;
            } else if (t[1] == "basis" && t.size() >= 11) {
                for (int i = 0; i < 3; ++i)
                    xf.basis.r[i] = {parseNumber(t[2 + static_cast<size_t>(i) * 3]),
                                     parseNumber(t[3 + static_cast<size_t>(i) * 3]),
                                     parseNumber(t[4 + static_cast<size_t>(i) * 3])};
            } else if (t[1] == "quat" && t.size() >= 6) {
                xf.basis = setRotation(Quat{parseNumber(t[2]), parseNumber(t[3]), parseNumber(t[4]),
                                            parseNumber(t[5])});
            } else if (t[1] == "inertia" && t.size() >= 11) {
                for (int i = 0; i < 3; ++i)
                    inertia.r[i] = {parseNumber(t[2 + static_cast<size_t>(i) * 3]),
                                    parseNumber(t[3 + static_cast<size_t>(i) * 3]),
                                    parseNumber(t[4 + static_cast<size_t>(i) * 3])};
                haveInertia = true;
            }
        }
    }
    if (!havePos || !haveVel || !haveAng) {
        std::fprintf(stderr, "scenario must set ball pos, vel and angvel\n");
        return 2;
    }

    rlios::io::ArenaCollisionData arena;
    bool haveArena = false;
    if (!collisionPath.empty()) {
        std::string error;
        haveArena = arena.load(collisionPath, error);
        if (!haveArena && needsCollision) {
            std::printf("SKIP %s: %s (collision data is never committed)\n", name.c_str(),
                        error.c_str());
            return 77;
        }
    } else if (needsCollision) {
        std::printf("SKIP %s: needs --collision\n", name.c_str());
        return 77;
    }

    static BallWorld w;
    initBallWorld(w, xf, linVel, angVel, haveArena ? &arena.collision() : nullptr);
    if (haveInertia)
        w.ball.invInertiaWorld = inertia;

    size_t divergingTicks = 0;
    size_t firstBad = 0;
    size_t compared = 0;
    for (size_t k = 0; k < rows.size(); ++k) {
        stepBallWorld(w);
        const Row& r = rows[k];
        if (!r.haveXf || !r.haveVel) {
            std::fprintf(stderr, "row %zu incomplete\n", k);
            return 2;
        }
        const BallBody& b = w.ball;
        const Field f[] = {
            {"basis[0].x", floatBits(b.xf.basis.r[0].x), r.xf[0]},
            {"basis[0].y", floatBits(b.xf.basis.r[0].y), r.xf[1]},
            {"basis[0].z", floatBits(b.xf.basis.r[0].z), r.xf[2]},
            {"basis[1].x", floatBits(b.xf.basis.r[1].x), r.xf[4]},
            {"basis[1].y", floatBits(b.xf.basis.r[1].y), r.xf[5]},
            {"basis[1].z", floatBits(b.xf.basis.r[1].z), r.xf[6]},
            {"basis[2].x", floatBits(b.xf.basis.r[2].x), r.xf[8]},
            {"basis[2].y", floatBits(b.xf.basis.r[2].y), r.xf[9]},
            {"basis[2].z", floatBits(b.xf.basis.r[2].z), r.xf[10]},
            {"pos.x", floatBits(b.xf.origin.x), r.xf[12]},
            {"pos.y", floatBits(b.xf.origin.y), r.xf[13]},
            {"pos.z", floatBits(b.xf.origin.z), r.xf[14]},
            {"linvel.x", floatBits(b.linVel.x), r.vel[0]},
            {"linvel.y", floatBits(b.linVel.y), r.vel[1]},
            {"linvel.z", floatBits(b.linVel.z), r.vel[2]},
            {"angvel.x", floatBits(b.angVel.x), r.vel[4]},
            {"angvel.y", floatBits(b.angVel.y), r.vel[5]},
            {"angvel.z", floatBits(b.angVel.z), r.vel[6]},
        };
        bool bad = false;
        for (const Field& fld : f) {
            if (fld.ours != fld.ref) {
                if (divergingTicks == 0 || static_cast<long>(k) < trace) {
                    std::printf("tick %zu %-10s ours 0x%08X (%.9g)  game 0x%08X (%.9g)\n", k + 1,
                                fld.name, fld.ours, static_cast<double>(bitsFloat(fld.ours)),
                                fld.ref, static_cast<double>(bitsFloat(fld.ref)));
                }
                bad = true;
            }
        }
        if (static_cast<long>(k) < trace) {
            std::printf("tick %zu z %.9g vz %.9g act %d contacts %d%s\n", k + 1,
                        static_cast<double>(b.xf.origin.z), static_cast<double>(b.linVel.z),
                        b.activation, totalContacts(w), bad ? "  DIVERGED" : "");
        }
        if (bad) {
            if (divergingTicks == 0)
                firstBad = k + 1;
            ++divergingTicks;
            if (trace == 0)
                break; // later ticks only compound the first error
        }
        ++compared;
    }

    if (divergingTicks == 0) {
        std::printf("PASS %s: %zu/%zu ticks bit-identical (basis, position, linear and angular "
                    "velocity), final hash %016llx, %s, %llu calls outside proven-exact math\n",
                    name.c_str(), compared, rows.size(),
                    static_cast<unsigned long long>(hashBallWorld(w)),
                    haveArena ? "planes + meshes" : "planes only", crtInexactCalls());
        return 0;
    }
    std::printf("FAIL %s: first diverging tick %zu of %zu (%zu ticks matched before it), %llu "
                "calls outside proven-exact math\n",
                name.c_str(), firstBad, rows.size(), firstBad - 1, crtInexactCalls());
    return 1;
}
