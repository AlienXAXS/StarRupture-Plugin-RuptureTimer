#include "wave/wave_types.h"

namespace RuptureTimer
{
	const char* StageName(WaveStage s)
	{
		switch (s)
		{
		case WaveStage::None:     return "Calm";
		case WaveStage::PreWave:  return "Pre-Wave";
		case WaveStage::Moving:   return "Moving";
		case WaveStage::Fadeout:  return "Fadeout";
		case WaveStage::Growback: return "Growback";
		}
		return "Unknown";
	}

	const char* StageShortName(WaveStage s)
	{
		switch (s)
		{
		case WaveStage::None:     return "CALM";
		case WaveStage::PreWave:  return "PRE";
		case WaveStage::Moving:   return "MOVE";
		case WaveStage::Fadeout:  return "FADE";
		case WaveStage::Growback: return "GROW";
		}
		return "?";
	}

	const char* WaveTypeName(WaveType t)
	{
		switch (t)
		{
		case WaveType::None: return "None";
		case WaveType::Heat: return "Heat";
		case WaveType::Cold: return "Cold";
		}
		return "Unknown";
	}

	const char* SubstageName(WaveStage stage, uint8_t substage)
	{
		if (substage == 0) return "";
		switch (stage)
		{
		case WaveStage::PreWave:
			switch (static_cast<PreWaveSubstage>(substage))
			{
			case PreWaveSubstage::BeforeExplosion: return "Before Explosion";
			case PreWaveSubstage::AfterExplosion:  return "After Explosion";
			default: break;
			}
			break;
		case WaveStage::Fadeout:
			switch (static_cast<FadeoutSubstage>(substage))
			{
			case FadeoutSubstage::FireWave: return "Fire Wave";
			case FadeoutSubstage::Burning:  return "Burning";
			case FadeoutSubstage::Fading:   return "Fading";
			default: break;
			}
			break;
		case WaveStage::Growback:
			switch (static_cast<GrowbackSubstage>(substage))
			{
			case GrowbackSubstage::MoonPhase:     return "Moon Phase";
			case GrowbackSubstage::RegrowthStart: return "Regrowth Start";
			case GrowbackSubstage::Regrowth:      return "Regrowth";
			default: break;
			}
			break;
		default:
			break;
		}
		return "";
	}

	const char* RoleName(SessionRole r)
	{
		switch (r)
		{
		case SessionRole::Unresolved:      return "...";
		case SessionRole::Solo:            return "SOLO";
		case SessionRole::ListenHost:      return "HOST";
		case SessionRole::DedicatedServer: return "SERVER";
		case SessionRole::RemoteClient:    return "CLIENT";
		}
		return "?";
	}

	const char* WaitReasonText(WaitReason r)
	{
		switch (r)
		{
		case WaitReason::None:                return "";
		case WaitReason::DetectingSession:    return "Detecting session type...";
		case WaitReason::NoWorld:             return "Waiting for the world to load...";
		case WaitReason::SubsystemUnresolved: return "Locating the wave subsystem...";
		case WaitReason::NoServerData:        return "Waiting for server data...";
		case WaitReason::LinkStale:           return "Server data has gone stale";
		}
		return "";
	}
}
