// Soccar rule constants. Every value names its source. Tags used below:
//   [CDO]   class default object, SDK dump of the running game (work/sdk/current,
//   tools/sdk/defaults.py) [ARCH]  archetype in GameInfo_Soccar_SF.upk,
//   Archetypes.GameEvent.GameEvent_Soccar / Archetypes.Car
//           (read with tools/extract/upk_actors.py' property reader)
//   [MAP]   placed actor in Stadium_P.upk (work/layout/Stadium_P_actors.json,
//   tools/extract/upk_actors.py) [BIN]   RocketLeague.exe, RVA given [RS]    RocketSim
//   (work/refs/RocketSim/src/RLConst.h). Cross-check only: values tagged only [RS]
//           are UNPROVEN until a source above confirms them.
#pragma once

#include <cstdint>

namespace rlios::game {

// --- Match flow -------------------------------------------------------------------------------
// [ARCH] GameEvent_Soccar.GameTime = 300 (int seconds).
inline constexpr int32_t kGameTimeSeconds = 300;
// [CDO] GameEvent_TA.CountdownTime = 3 (seconds of frozen kickoff countdown).
inline constexpr int32_t kCountdownSeconds = 3;
// [ARCH] GameEvent_Soccar.PostGoalTime = 3.0 (0x40400000): goal celebration before the reset.
// Replays are skipped (CLAUDE.md: replay-free match flow).
inline constexpr float kPostGoalSeconds = 3.0f;
// [CDO] GameEvent_Soccar_TA.BallHasBeenHitStartDelay = 5.0 (0x40a00000). Used here as: after "Go",
// the clock starts at the first ball touch or after this delay. UNPROVEN usage (UScript not
// decompiled).
inline constexpr float kBallHasBeenHitStartDelay = 5.0f;
// [ARCH] GameEvent_Soccar.FinishTime = 60 (post-match podium seconds; we only flag "ended").
inline constexpr int32_t kFinishTimeSeconds = 60;
// [ARCH] GameEvent_Soccar.SecondsRemainingCountdown = 10 (HUD countdown from 10 s).
inline constexpr int32_t kSecondsRemainingCountdown = 10;

// --- Boost (CarComponent_Boost_TA) -------------------------------------------------------------
// Units are fractions of a full tank: [MAP] small pad BoostAmount = 0.12 (0x3df5c28f) is RLBot's
// "12 boost", so a full tank is 1.0. Max = 1.0 is consistent with that, but the archetype
// Archetypes.CarComponents.CarComponent_Boost (MaxBoostAmount, StartBoostAmount,
// BoostConsumptionRate, MinBoostTime) has not been located yet: those four are [RS] UNPROVEN.
inline constexpr float kMaxBoost = 1.0f;              // [RS] BOOST_MAX 100 / 100
inline constexpr float kStartBoost = 1.0f / 3.0f;     // [RS] BOOST_SPAWN_AMOUNT = BOOST_MAX/3
inline constexpr float kBoostPerSecond = 1.0f / 3.0f; // [RS] BOOST_USED_PER_SECOND = BOOST_MAX/3
inline constexpr float kMinBoostTime = 0.1f;          // [RS] BOOST_MIN_TIME
// [BIN] CarComponent_Boost_TA.SetBoostAmount, RVA 0xEEFD40: clamp to [0, MaxBoostAmount], then
// store only if |new - current| >= 1e-8 (const at RVA 0x1B0F430 = 0x322bcc77) or new == 0.
inline constexpr uint32_t kBoostSetEpsilonBits = 0x322bcc77u;

// --- Boost pads (VehiclePickup_Boost_TA) -------------------------------------------------------
// [MAP] big pads: BoostAmount 9999 (0x461c3c00, clamped to max by SetBoostAmount), RespawnDelay 10,
// CylinderComponent r=160 h=64. Small pads: BoostAmount 0.12, RespawnDelay 4, r=128 h=64.
inline constexpr float kBigPadAmount = 9999.0f;
inline constexpr float kSmallPadAmount = 0.12f;
inline constexpr float kBigPadRespawn = 10.0f;
inline constexpr float kSmallPadRespawn = 4.0f;
inline constexpr float kBigPadRadius = 160.0f;
inline constexpr float kSmallPadRadius = 128.0f;
inline constexpr float kPadHeight = 64.0f; // CollisionHeight (UE3: half height)

// --- Demolition (Car_TA) -----------------------------------------------------------------------
// [CDO] Car_TA.DemolishTarget = 1, DemolishSpeed = 5. Enum declaration order is not in the dump
// (names only); read as OtherTeam / Supersonic, which matches the standard rules. UNPROVEN order.
// [RS] DEMO_RESPAWN_TIME = 3 s, UNPROVEN.
inline constexpr float kDemoRespawnSeconds = 3.0f;

// --- Goals (GoalVolume_TA) ---------------------------------------------------------------------
// [MAP] GoalVolume_TA_0 (TeamNum 0, blue goal, -y) and GoalVolume_TA_4 (TeamNum 1, orange, +y):
// convex brush box x in [-960, 960], |y| in [5120, 6032], z in [-64, 688].
inline constexpr float kGoalHalfWidth = 960.0f;
inline constexpr float kGoalLineY = 5120.0f;
inline constexpr float kGoalBackY = 6032.0f;
inline constexpr float kGoalMinZ = -64.0f;
inline constexpr float kGoalMaxZ = 688.0f;
// Goal counts once the whole ball is behind the goal line: |y| > 5120 + ball radius. UNPROVEN
// (RS uses 5124.25 + radius; the native touch test of GoalVolume_TA is not reversed yet).
// [BIN] ball radius 91.25 uu (ball_world.cpp kBallRadius, measured).
inline constexpr float kBallRadius = 91.25f;

// --- Overtime / match end ----------------------------------------------------------------------
// At 0:00 the match ends once the ball touches the ground (it is "in the air" otherwise).
// Ground test: ball z <= radius + kBallGroundSlack. UNPROVEN threshold.
inline constexpr float kBallGroundSlack = 2.0f;

} // namespace rlios::game
