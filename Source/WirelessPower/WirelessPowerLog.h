// Copyright Remake of Feyko's Wireless Power. All Rights Reserved.

#pragma once

// ---------------------------------------------------------------------------------------------
// v1.0.29 - runtime-logging master switch.  This header is the ONLY place the switch lives.
//
// WHY: the user asked for a quiet log ("日志不记录"). v1.0.28 flooded FactoryGame.log -- a [diag]
// status dump every 3 s per building, PLUS a "linked peer" / "broadcasting to machine" line per
// link per validation pass (measured: ~8 lines per second on a small test grid, and the log file
// already reached 350 KB within a minute of loading a save).
//
// HOW: every Log-level message in the mod now goes through WP_LOGF(). While WP_LOG_VERBOSE is
// false the whole statement compiles away -- and because it is an `if`, the ARGUMENTS are not
// evaluated either, so the FString/Printf work the old logging did every pass disappears with it
// (that cost is real: the [diag] block built two FStrings per building per 3 s on the game thread).
//
// TO DEBUG: set WP_LOG_VERBOSE to true, repackage, reproduce, read the log, set it back to false.
//
// DELIBERATELY NOT GATED:
//   * Warning-level calls -- they only fire when something is genuinely wrong (missing build
//     category, schematic manager not ready, ...) and are never high-frequency.
//   * the one-line load banner in WirelessPowerModule.cpp -- it is the fingerprint that tells us
//     which build the game actually loaded.
// ---------------------------------------------------------------------------------------------
static constexpr bool WP_LOG_VERBOSE = false;

#define WP_LOGF(...) do { if (WP_LOG_VERBOSE) { UE_LOG(LogWirelessPower, Log, __VA_ARGS__); } } while (0)

// ---------------------------------------------------------------------------------------------
// v1.0.32 - TEMPORARY verification switch, turned OFF again for the v1.0.33 release.
//
// WHY it existed: the laser beam switched from "stretched cube instances" to Feyko's original
// mechanism (UParticleSystemComponent + P_Laser + SetBeamSourcePoint/SetBeamTargetPoint). His
// P_Laser is a pre-compiled asset we cannot read from Python (UParticleSystem::Emitters is not
// editor-exposed), so the ONLY way to learn what it actually contains -- beam emitter count,
// MaxBeamCount, the source/target method flags, the particle colour and size -- was to dump it
// from C++ at runtime. It fired exactly ONCE per process (guarded by a file-static).
//
// The dump answered the questions (maxBeamCount=1, peakActive=3, source/target already
// UserSet, StartColor not a constant distribution), so the switch is off for release.
// ---------------------------------------------------------------------------------------------
static constexpr bool WP_LOG_VERIFY = false;

#define WP_LOGV(...) do { if (WP_LOG_VERIFY) { UE_LOG(LogWirelessPower, Warning, __VA_ARGS__); } } while (0)
