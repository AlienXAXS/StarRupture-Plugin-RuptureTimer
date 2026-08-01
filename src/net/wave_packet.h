#pragma once

#include <cstdint>

namespace RuptureTimer
{
	// Wire format. Both DLL builds must agree byte for byte — the modloader routes
	// on plugin name + typeid(T).name(), so a mismatch here silently drops packets.
	// Bump kSchemaVersion whenever a field changes meaning.
	// v2: added the raw source fields so the diagnostics window on a client
	//     reports what the SERVER actually read, instead of its own zeroed
	//     defaults. Without them a remote session cannot be debugged at all.
	constexpr uint8_t kSchemaVersion = 2;

	enum PacketFlags : uint16_t
	{
		PF_None               = 0,
		PF_WaveInProgress     = 1 << 0,
		PF_Paused             = 1 << 1,
		PF_StageTimeValid     = 1 << 2,
		PF_NextWaveTimeValid  = 1 << 3,
		PF_SubstageTimeValid  = 1 << 4,
		PF_PositionValid      = 1 << 5,
		PF_SubsystemResolved  = 1 << 6,
		PF_TimerActorResolved = 1 << 7,
		PF_SourcesDisagree    = 1 << 8,
		PF_WavesStopped       = 1 << 9,
		PF_TimerSubsysResolved = 1 << 10,
		PF_NextWaveOverdue   = 1 << 11,
		PF_NextWaveStale     = 1 << 12,
	};

#pragma pack(push, 1)

	// 40 bytes; sent at most a few times a second.
	struct RuptureWavePacket
	{
		uint8_t  schemaVersion;
		uint8_t  waveType;
		uint8_t  stage;
		uint8_t  nextStage;
		uint8_t  substage;
		uint8_t  nextSubstage;
		uint16_t flags;
		uint32_t sequence;
		float    stageProgress;
		float    stageDuration;
		float    stageRemaining;
		float    substageRemaining;
		float    nextWaveIn;
		float    wavePositionX;
		float    timeSinceLastWave;

		// Raw source values, purely so the diagnostics window on a remote client
		// reports what the SERVER read rather than its own zeroed defaults.
		float    rawNextTime;        // ACrWaveTimerActor::NextTime (absolute)
		int32_t  rawNextPhase;       // ACrWaveTimerActor::NextPhase
		float    rawTimerRemaining;  // NextTime - now, before validity checks
		float    rawCalmTimer;       // UCrEnviroWaveTimerSubsystem::NextWaveTimer
	};

	// Client -> server: "I just joined, send me a snapshot now." Keeps a new
	// player from staring at the waiting state for a whole broadcast interval.
	struct RuptureHelloPacket
	{
		uint8_t schemaVersion;
		uint8_t reserved[3];
	};

#pragma pack(pop)

	static_assert(sizeof(RuptureWavePacket) == 56, "RuptureWavePacket layout changed");
	static_assert(sizeof(RuptureHelloPacket) == 4, "RuptureHelloPacket layout changed");
}
