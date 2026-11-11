#include "game/scenario.h"

#include <cstdlib>
#include <sstream>

namespace rlios::game {

namespace {
bool parseFloat(const std::string& tok, float& out) {
    char* end = nullptr;
    out = std::strtof(tok.c_str(), &end); // correctly rounded on glibc and Apple libc
    return end != tok.c_str() && *end == '\0';
}
} // namespace

bool parseScenario(const std::string& text, Scenario& out, std::string& err) {
    out = Scenario{};
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    bool haveTicks = false;
    while (std::getline(in, line)) {
        ++lineNo;
        const auto hash = line.find('#');
        if (hash != std::string::npos)
            line.erase(hash);
        std::istringstream ls(line);
        std::string cmd;
        if (!(ls >> cmd))
            continue;
        if (cmd == "ticks") {
            unsigned long long n = 0;
            if (!(ls >> n)) {
                err = "line " + std::to_string(lineNo) + ": bad ticks";
                return false;
            }
            out.ticks = n;
            haveTicks = true;
        } else if (cmd == "input") {
            unsigned long long t = 0;
            std::string ax, ay;
            InputEvent ev{};
            if (!(ls >> t >> ax >> ay) || !parseFloat(ax, ev.input.ax) ||
                !parseFloat(ay, ev.input.ay)) {
                err = "line " + std::to_string(lineNo) + ": bad input";
                return false;
            }
            ev.tick = t;
            if (!out.inputs.empty() && out.inputs.back().tick > ev.tick) {
                err = "line " + std::to_string(lineNo) + ": inputs not sorted";
                return false;
            }
            out.inputs.push_back(ev);
        } else {
            err = "line " + std::to_string(lineNo) + ": unknown command '" + cmd + "'";
            return false;
        }
    }
    if (!haveTicks) {
        err = "missing 'ticks'";
        return false;
    }
    return true;
}

void runScenario(const Scenario& sc, const std::function<void(const sim::State&)>& onTick) {
    sim::State s = sim::initialState();
    sim::Input cur{0.0f, 0.0f};
    size_t next = 0;
    onTick(s);
    for (uint64_t t = 0; t < sc.ticks; ++t) {
        while (next < sc.inputs.size() && sc.inputs[next].tick <= t)
            cur = sc.inputs[next++].input;
        sim::step(s, cur);
        onTick(s);
    }
}

} // namespace rlios::game
