#pragma once

#include "wave/wave_types.h"

namespace RuptureTimer
{
	// Stage cycle: PreWave -> Moving -> Fadeout -> Growback -> None (calm) -> PreWave.
	WaveStage NextStageOf(WaveStage current);

	// Total seconds the given stage runs for, derived from the wave's settings.
	// `s` points at the game's FCrEnviroWaveSettings (read in place, member by
	// member, never copied).
	// Returns <= 0 when the settings cannot produce a sane duration (zero wave
	// speed, uninitialised struct); callers must treat that as "unknown".
	float StageDuration(WaveStage stage, const void* s);

	// Substage that follows `substage` within `stage`, or 0 when it is the last one.
	uint8_t NextSubstageOf(WaveStage stage, uint8_t substage);

	// Seconds left in the current substage, given progress through the whole stage.
	// Returns false when the stage has no substages or the durations don't add up.
	bool SubstageRemaining(WaveStage stage, uint8_t substage, float stageProgress,
	                       const void* s, float* outRemaining);

	// A duration/countdown is only trusted if it is finite, non-negative and inside
	// a sane bound — the wave cycle is minutes, so anything past a day is garbage.
	bool IsSaneSeconds(float v);
}
