// DFH Stadium (Stadium_P) gameplay layout: boost pads and player starts.
// Source: placed actors of Stadium_P.upk (tools/extract/upk_actors.py -> work/layout/
// Stadium_P_actors.json). Floats are the exact bit patterns of the actor Location properties.
#pragma once

#include <cstdint>

#include "sim/vec.h"

namespace rlios::game {

struct PadSpec {
    sim::Vec3 pos; // uu
    bool big;
};

struct StartSpec {
    sim::Vec3 pos; // uu (actor location, cylinder centre: z = 83)
    int32_t yawUU; // UE3 rotator yaw (65536 = 360 deg)
};

inline constexpr int kNumPads = 34;
extern const PadSpec kStadiumPads[kNumPads];

// PlayerStart_TA with bPrimaryStart (default true): the five kickoff spots, blue side.
inline constexpr int kNumKickoffStarts = 5;
extern const StartSpec kKickoffStarts[kNumKickoffStarts];
// PlayerStart_TA with bPrimaryStart = False: the four demolition respawn spots, blue side.
inline constexpr int kNumRespawnStarts = 4;
extern const StartSpec kRespawnStarts[kNumRespawnStarts];

// Orange uses the blue spots rotated 180 degrees about the field centre (the map holds blue-side
// starts only). Position (-x, -y, z), yaw + 32768.
StartSpec mirrorForOrange(const StartSpec& s);

} // namespace rlios::game
