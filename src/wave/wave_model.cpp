#include "wave/wave_model.h"

#include "Chimera_structs.hpp"

#include <cmath>

namespace RuptureTimer
{
	namespace
	{
		constexpr float kMaxSaneSeconds = 86400.0f;

		// Substage durations in running order, for the three stages that have them.
		// Order matches the game: it advances through these as stage progress runs 0->1.
		int CollectSubstageDurations(WaveStage stage, const SDK::FCrEnviroWaveSettings& s,
		                             float out[3])
		{
			switch (stage)
			{
			case WaveStage::PreWave:
				// PreWaveDuration is the whole stage; the explosion is the tail of it.
				out[0] = s.PreWaveDuration - s.WavePreWaveExplosionDuration;
				out[1] = s.WavePreWaveExplosionDuration;
				return 2;
			case WaveStage::Fadeout:
				out[0] = s.WaveFadeoutFireWaveDuration;
				out[1] = s.WaveFadeoutBurningDuration;
				out[2] = s.WaveFadeoutFadingDuration;
				return 3;
			case WaveStage::Growback:
				out[0] = s.WaveGrowbackMoonPhaseDuration;
				out[1] = s.WaveGrowbackRegrowthStartDuration;
				out[2] = s.WaveGrowbackRegrowthDuration;
				return 3;
			default:
				return 0;
			}
		}
	}

	bool IsSaneSeconds(float v)
	{
		return std::isfinite(v) && v >= 0.0f && v <= kMaxSaneSeconds;
	}

	WaveStage NextStageOf(WaveStage current)
	{
		switch (current)
		{
		case WaveStage::PreWave:  return WaveStage::Moving;
		case WaveStage::Moving:   return WaveStage::Fadeout;
		case WaveStage::Fadeout:  return WaveStage::Growback;
		case WaveStage::Growback: return WaveStage::None;
		case WaveStage::None:     return WaveStage::PreWave;
		}
		return WaveStage::None;
	}

	float StageDuration(WaveStage stage, const SDK::FCrEnviroWaveSettings& s)
	{
		float d = 0.0f;
		switch (stage)
		{
		case WaveStage::PreWave:
			d = s.PreWaveDuration;
			break;
		case WaveStage::Moving:
			// The moving stage lasts however long the wave takes to cross the map.
			if (std::isfinite(s.WaveSpeed) && std::fabs(s.WaveSpeed) > 1e-6f)
				d = std::fabs(s.WaveEndPosition - s.WaveStartPosition) / std::fabs(s.WaveSpeed);
			break;
		case WaveStage::Fadeout:
			d = s.WaveFadeoutFireWaveDuration + s.WaveFadeoutBurningDuration + s.WaveFadeoutFadingDuration;
			break;
		case WaveStage::Growback:
			d = s.WaveGrowbackMoonPhaseDuration + s.WaveGrowbackRegrowthStartDuration + s.WaveGrowbackRegrowthDuration;
			break;
		case WaveStage::None:
			break;
		}
		return IsSaneSeconds(d) && d > 0.0f ? d : 0.0f;
	}

	uint8_t NextSubstageOf(WaveStage stage, uint8_t substage)
	{
		uint8_t last = 0;
		switch (stage)
		{
		case WaveStage::PreWave:  last = 2; break;
		case WaveStage::Fadeout:  last = 3; break;
		case WaveStage::Growback: last = 3; break;
		default: return 0;
		}
		return (substage >= 1 && substage < last) ? static_cast<uint8_t>(substage + 1) : 0;
	}

	bool SubstageRemaining(WaveStage stage, uint8_t substage, float stageProgress,
	                       const SDK::FCrEnviroWaveSettings& s, float* outRemaining)
	{
		if (!outRemaining || substage == 0) return false;

		float durations[3] = { 0.0f, 0.0f, 0.0f };
		const int count = CollectSubstageDurations(stage, s, durations);
		if (count == 0 || substage > count) return false;

		float total = 0.0f;
		for (int i = 0; i < count; ++i)
		{
			if (!IsSaneSeconds(durations[i])) return false;
			total += durations[i];
		}
		if (total <= 0.0f) return false;

		// Stage progress is stage-local, so elapsed time maps straight onto the
		// concatenated substage timeline.
		const float elapsed = stageProgress * total;
		float substageEnd = 0.0f;
		for (int i = 0; i < substage; ++i) substageEnd += durations[i];

		const float remaining = substageEnd - elapsed;
		if (!IsSaneSeconds(remaining)) return false;

		*outRemaining = remaining;
		return true;
	}
}
