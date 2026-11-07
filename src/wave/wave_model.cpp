#include "wave/wave_model.h"
#include "wave/wave_fields.h"

#include "Chimera_structs.hpp"

#include <cmath>

namespace RuptureTimer
{
	namespace
	{
		float Seconds(const PluginField<float>& field, const void* s)
		{
			return Fields::ValueOr<float>(field, s);
		}

		constexpr float kMaxSaneSeconds = 86400.0f;

		// Substage durations in running order, for the three stages that have them.
		// Order matches the game: it advances through these as stage progress runs 0->1.
		int CollectSubstageDurations(WaveStage stage, const void* s,
		                             float out[3])
		{
			switch (stage)
			{
			case WaveStage::PreWave:
				// PreWaveDuration is the whole stage; the explosion is the tail of it.
				out[0] = Seconds(Fields::PreWaveDuration, s) - Seconds(Fields::PreWaveExplosionDuration, s);
				out[1] = Seconds(Fields::PreWaveExplosionDuration, s);
				return 2;
			case WaveStage::Fadeout:
				out[0] = Seconds(Fields::FadeoutFireWaveDuration, s);
				out[1] = Seconds(Fields::FadeoutBurningDuration, s);
				out[2] = Seconds(Fields::FadeoutFadingDuration, s);
				return 3;
			case WaveStage::Growback:
				out[0] = Seconds(Fields::GrowbackMoonPhaseDuration, s);
				out[1] = Seconds(Fields::GrowbackRegrowthStartDuration, s);
				out[2] = Seconds(Fields::GrowbackRegrowthDuration, s);
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

	float StageDuration(WaveStage stage, const void* s)
	{
		float d = 0.0f;
		switch (stage)
		{
		case WaveStage::PreWave:
			d = Seconds(Fields::PreWaveDuration, s);
			break;
		case WaveStage::Moving:
			// The moving stage lasts however long the wave takes to cross the map.
			{
				const float speed = Seconds(Fields::WaveSpeed, s);
				if (std::isfinite(speed) && std::fabs(speed) > 1e-6f)
					d = std::fabs(Seconds(Fields::WaveEndPosition, s) - Seconds(Fields::WaveStartPosition, s)) / std::fabs(speed);
			}
			break;
		case WaveStage::Fadeout:
			d = Seconds(Fields::FadeoutFireWaveDuration, s) + Seconds(Fields::FadeoutBurningDuration, s) + Seconds(Fields::FadeoutFadingDuration, s);
			break;
		case WaveStage::Growback:
			d = Seconds(Fields::GrowbackMoonPhaseDuration, s) + Seconds(Fields::GrowbackRegrowthStartDuration, s) + Seconds(Fields::GrowbackRegrowthDuration, s);
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
	                       const void* s, float* outRemaining)
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
