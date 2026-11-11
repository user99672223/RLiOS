#include "game/arena.h"

namespace rlios::game {

using sim::bitsFloat;

// VehiclePickup_Boost_TA actors in export order. Big: BoostAmount 9999, RespawnDelay 10, r=160.
// Small: BoostAmount 0.12, RespawnDelay 4, r=128 (all h=64; checked by the extraction script).
const PadSpec kStadiumPads[kNumPads] = {
    {{bitsFloat(0xc5400000), bitsFloat(0x45800000), bitsFloat(0x42900000)},
     true}, // _0 (-3072, 4096, 72)
    {{bitsFloat(0x45400000), bitsFloat(0xc5800000), bitsFloat(0x42900000)},
     true}, // _1 (3072, -4096, 72)
    {{bitsFloat(0x45000000), bitsFloat(0xc4818000), bitsFloat(0x428029fe)},
     false}, // _15 (2048, -1036)
    {{bitsFloat(0xc4df8000), bitsFloat(0xc50fe000), bitsFloat(0x428029fe)},
     false}, // _16 (-1788, -2302)
    {{bitsFloat(0xc5400000), bitsFloat(0xc5800000), bitsFloat(0x42900000)},
     true}, // _2 (-3072, -4096, 72)
    {{bitsFloat(0x00000000), bitsFloat(0xc5300000), bitsFloat(0x428029fe)},
     false}, // _24 (0, -2816)
    {{bitsFloat(0xc5000000), bitsFloat(0xc4818000), bitsFloat(0x428029fe)},
     false}, // _25 (-2048, -1036)
    {{bitsFloat(0xc5600000), bitsFloat(0xc51b4000), bitsFloat(0x428029fe)},
     false}, // _26 (-3584, -2484)
    {{bitsFloat(0x44df8000), bitsFloat(0xc50fe000), bitsFloat(0x428029fe)},
     false}, // _27 (1788, -2302)
    {{bitsFloat(0xc4e00000), bitsFloat(0xc582c000), bitsFloat(0x428029fe)},
     false}, // _29 (-1792, -4184)
    {{bitsFloat(0x44e00000), bitsFloat(0xc582c000), bitsFloat(0x428029fe)},
     false}, // _30 (1792, -4184)
    {{bitsFloat(0xc46b0000), bitsFloat(0xc54ec000), bitsFloat(0x428029fe)},
     false}, // _37 (-940, -3308)
    {{bitsFloat(0x446b0000), bitsFloat(0xc54ec000), bitsFloat(0x428029fe)},
     false}, // _44 (940, -3308)
    {{bitsFloat(0x45600000), bitsFloat(0xc51b4000), bitsFloat(0x428029fe)},
     false}, // _45 (3584, -2484)
    {{bitsFloat(0x00000000), bitsFloat(0x44800000), bitsFloat(0x428029fe)}, false}, // _46 (0, 1024)
    {{bitsFloat(0xc5000000), bitsFloat(0x44818000), bitsFloat(0x428029fe)},
     false}, // _47 (-2048, 1036)
    {{bitsFloat(0xc4df8000), bitsFloat(0x450fe000), bitsFloat(0x428029fe)},
     false}, // _48 (-1788, 2302)
    {{bitsFloat(0x45000000), bitsFloat(0x44818000), bitsFloat(0x428029fe)},
     false}, // _49 (2048, 1036)
    {{bitsFloat(0x44df8000), bitsFloat(0x450fe000), bitsFloat(0x428029fe)},
     false}, // _50 (1788, 2302)
    {{bitsFloat(0x45600000), bitsFloat(0x451b4000), bitsFloat(0x428029fe)},
     false}, // _51 (3584, 2484)
    {{bitsFloat(0x44e00000), bitsFloat(0x4582c000), bitsFloat(0x428029fe)},
     false}, // _52 (1792, 4184)
    {{bitsFloat(0xc4e00000), bitsFloat(0x4582c000), bitsFloat(0x428029fe)},
     false}, // _53 (-1792, 4184)
    {{bitsFloat(0x00000000), bitsFloat(0x45300000), bitsFloat(0x428029fe)}, false}, // _54 (0, 2816)
    {{bitsFloat(0xc46b0000), bitsFloat(0x454ec000), bitsFloat(0x428029fe)},
     false}, // _55 (-940, 3308)
    {{bitsFloat(0xc5600000), bitsFloat(0x451b4000), bitsFloat(0x428029fe)},
     false}, // _56 (-3584, 2484)
    {{bitsFloat(0x446b0000), bitsFloat(0x454ec000), bitsFloat(0x428029fe)},
     false}, // _57 (940, 3308)
    {{bitsFloat(0x00000000), bitsFloat(0x45848000), bitsFloat(0x428029fe)}, false}, // _58 (0, 4240)
    {{bitsFloat(0x44800000), bitsFloat(0x00000000), bitsFloat(0x428029fe)}, false}, // _59 (1024, 0)
    {{bitsFloat(0xc5600000), bitsFloat(0x00000000), bitsFloat(0x42900000)},
     true}, // _6 (-3584, 0, 72)
    {{bitsFloat(0x00000000), bitsFloat(0xc4800000), bitsFloat(0x428029fe)},
     false}, // _60 (0, -1024)
    {{bitsFloat(0xc4800000), bitsFloat(0x00000000), bitsFloat(0x428029fe)},
     false}, // _61 (-1024, 0)
    {{bitsFloat(0x00000000), bitsFloat(0xc5848000), bitsFloat(0x428029fe)},
     false}, // _62 (0, -4240)
    {{bitsFloat(0x45600000), bitsFloat(0x00000000), bitsFloat(0x42900000)},
     true}, // _8 (3584, 0, 72)
    {{bitsFloat(0x45400000), bitsFloat(0x45800000), bitsFloat(0x42900000)},
     true}, // _9 (3072, 4096, 72)
};

// Kickoff order matches RLBot's published list (cross-check): diagonal L/R, off-centre L/R, back.
const StartSpec kKickoffStarts[kNumKickoffStarts] = {
    {{bitsFloat(0xc5000000), bitsFloat(0xc5200000), bitsFloat(0x42a5ffff)},
     8192}, // _1 (-2048,-2560)
    {{bitsFloat(0x45000000), bitsFloat(0xc5200000), bitsFloat(0x42a5ffff)},
     24576}, // _10 (2048,-2560)
    {{bitsFloat(0xc3800000), bitsFloat(0xc5700000), bitsFloat(0x42a5ffff)},
     16384}, // _2 (-256,-3840)
    {{bitsFloat(0x43800000), bitsFloat(0xc5700000), bitsFloat(0x42a5ffff)},
     16384}, // _9 (256,-3840)
    {{bitsFloat(0x00000000), bitsFloat(0xc5900000), bitsFloat(0x42a5ffff)}, 16384}, // _8 (0,-4608)
};

const StartSpec kRespawnStarts[kNumRespawnStarts] = {
    {{bitsFloat(0xc5100000), bitsFloat(0xc5900000), bitsFloat(0x42a5ffff)},
     16384}, // _4 (-2304,-4608)
    {{bitsFloat(0xc5280000), bitsFloat(0xc5900000), bitsFloat(0x42a5ffff)},
     16384}, // _3 (-2688,-4608)
    {{bitsFloat(0x45100000), bitsFloat(0xc5900000), bitsFloat(0x42a5ffff)},
     16384}, // _5 (2304,-4608)
    {{bitsFloat(0x45280000), bitsFloat(0xc5900000), bitsFloat(0x42a5ffff)},
     16384}, // _0 (2688,-4608)
};

StartSpec mirrorForOrange(const StartSpec& s) {
    StartSpec o = s;
    o.pos.x = -s.pos.x;
    o.pos.y = -s.pos.y;
    o.yawUU = (s.yawUU + 32768) & 65535;
    return o;
}

} // namespace rlios::game
