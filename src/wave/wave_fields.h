#pragma once

#include "plugin_interface.h"
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include "Chimera_structs.hpp"
#include "plugin_map.h"

#include <iterator>
#include <vector>

// Every member of a game class the wave reader reads, taken from the loader's
// map for the running game build (see plugin_map.h). Resolved once in
// OnPluginLoadHooks; read on the game thread with In(object), which is null
// when the object is null or the member did not resolve.

#define RUPTURETIMER_FIELD(Class, Member) SR_FIELD(decltype(SDK::Class::Member), Class, Member, 0)

namespace RuptureTimer::Fields
{
	inline auto WaveTimerActor = RUPTURETIMER_FIELD(ACrGameStateBase, WaveTimerActor);

	inline auto NextTime  = RUPTURETIMER_FIELD(ACrWaveTimerActor, NextTime);
	inline auto NextPhase = RUPTURETIMER_FIELD(ACrWaveTimerActor, NextPhase);
	inline auto Pause     = RUPTURETIMER_FIELD(ACrWaveTimerActor, bPause);

	inline auto PreWaveSubstage  = RUPTURETIMER_FIELD(UCrEnviroWaveSubsystem, CurrentPreWaveSubstage);
	inline auto FadeoutSubstage  = RUPTURETIMER_FIELD(UCrEnviroWaveSubsystem, CurrentFadeoutSubstage);
	inline auto GrowbackSubstage = RUPTURETIMER_FIELD(UCrEnviroWaveSubsystem, CurrentGrowbackSubstage);

	// GetCurrentStageSettings returns the struct by value, so its size is checked
	// against the running build, and wave_model.cpp reads its members by name.
	inline auto WaveSettingsSize = SR_SIZE_CHECK(FCrEnviroWaveSettings);

	inline auto WaveSpeed                     = RUPTURETIMER_FIELD(FCrEnviroWaveSettings, WaveSpeed);
	inline auto WaveStartPosition             = RUPTURETIMER_FIELD(FCrEnviroWaveSettings, WaveStartPosition);
	inline auto WaveEndPosition               = RUPTURETIMER_FIELD(FCrEnviroWaveSettings, WaveEndPosition);
	inline auto PreWaveDuration               = RUPTURETIMER_FIELD(FCrEnviroWaveSettings, PreWaveDuration);
	inline auto PreWaveExplosionDuration      = RUPTURETIMER_FIELD(FCrEnviroWaveSettings, WavePreWaveExplosionDuration);
	inline auto FadeoutFireWaveDuration       = RUPTURETIMER_FIELD(FCrEnviroWaveSettings, WaveFadeoutFireWaveDuration);
	inline auto FadeoutBurningDuration        = RUPTURETIMER_FIELD(FCrEnviroWaveSettings, WaveFadeoutBurningDuration);
	inline auto FadeoutFadingDuration         = RUPTURETIMER_FIELD(FCrEnviroWaveSettings, WaveFadeoutFadingDuration);
	inline auto GrowbackMoonPhaseDuration     = RUPTURETIMER_FIELD(FCrEnviroWaveSettings, WaveGrowbackMoonPhaseDuration);
	inline auto GrowbackRegrowthStartDuration = RUPTURETIMER_FIELD(FCrEnviroWaveSettings, WaveGrowbackRegrowthStartDuration);
	inline auto GrowbackRegrowthDuration      = RUPTURETIMER_FIELD(FCrEnviroWaveSettings, WaveGrowbackRegrowthDuration);

	// UCrEnviroWaveTimerSubsystem's members sit inside the SDK's Pad_30[0x60], so
	// they have no names there: native rows. The rows below are CL-127004's, the
	// build this plugin was compiled against; every other build comes from the
	// loader's map. WaitingDuration (0x78) is not a member any more on
	// CL-128786, so it is not requested. NextWaveTimer is the live countdown:
	// the subsystem's Tick decrements it while bWaitingForNextWave is set and
	// waves are not paused.
	inline auto TimerNextWaveTimer = PluginField<float>{ { "UCrEnviroWaveTimerSubsystem::NextWaveTimer",
		"UCrEnviroWaveTimerSubsystem", "NextWaveTimer", PLUGIN_FIELD_NATIVE, sizeof(float), 0x7C } };
	inline auto TimerWaitingForNextWave = PluginField<bool>{ { "UCrEnviroWaveTimerSubsystem::bWaitingForNextWave",
		"UCrEnviroWaveTimerSubsystem", "bWaitingForNextWave", PLUGIN_FIELD_NATIVE, sizeof(bool), 0x80 } };
	inline auto TimerStopWaves = PluginField<bool>{ { "UCrEnviroWaveTimerSubsystem::bStopWaves",
		"UCrEnviroWaveTimerSubsystem", "bStopWaves", PLUGIN_FIELD_NATIVE, sizeof(bool), 0x83 } };
	inline auto TimerActor = PluginField<SDK::ACrWaveTimerActor*>{ { "UCrEnviroWaveTimerSubsystem::TimerActor",
		"UCrEnviroWaveTimerSubsystem", "TimerActor", PLUGIN_FIELD_NATIVE, sizeof(void*), 0x90 } };

	inline const PluginFieldCandidate kNextWaveTimerRows[] = {
		{ 127004, 0x7C, sizeof(float), 0, nullptr },
	};
	inline const PluginFieldCandidate kWaitingForNextWaveRows[] = {
		{ 127004, 0x80, sizeof(bool), 0, nullptr },
	};
	inline const PluginFieldCandidate kStopWavesRows[] = {
		{ 127004, 0x83, sizeof(bool), 0, nullptr },
	};
	inline const PluginFieldCandidate kTimerActorRows[] = {
		{ 127004, 0x90, sizeof(void*), 0, nullptr },
	};

	// The SDK's helpers, under the names the plugin uses.
	using PluginMap::KeysOf;
	using PluginMap::ValueOr;

	// The whole timer reads these members, so the plugin has one feature. Off,
	// the plugin loads but reads no wave state, shows no overlay and draws
	// nothing; the overlay and network config entries are greyed out.
	inline PluginMap::Feature WaveTimer{ "timer", "Rupture timer overlay",
		KeysOf(WaveTimerActor, NextTime, NextPhase, Pause,
		       PreWaveSubstage, FadeoutSubstage, GrowbackSubstage,
		       WaveSettingsSize, WaveSpeed, WaveStartPosition, WaveEndPosition, PreWaveDuration,
		       PreWaveExplosionDuration, FadeoutFireWaveDuration, FadeoutBurningDuration,
		       FadeoutFadingDuration, GrowbackMoonPhaseDuration, GrowbackRegrowthStartDuration,
		       GrowbackRegrowthDuration, TimerNextWaveTimer, TimerWaitingForNextWave, TimerStopWaves, TimerActor),
		{ "General.ShowOverlay", "General.ToggleKey", "Display.*", "Network.*" } };

	// Call before Resolve, so a miss lands on the feature and not on the plugin.
	inline void DeclareFeatures(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		PluginMap::DeclareFeatures(scanner, self, WaveTimer);
	}

	// Asks the loader for every row above. False when any did not resolve; the
	// loader has then recorded the failure and refuses the plugin.
	inline bool Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		bool all = PluginMap::ResolveFields(scanner, self,
			WaveTimerActor, NextTime, NextPhase, Pause,
			PreWaveSubstage, FadeoutSubstage, GrowbackSubstage, WaveSettingsSize,
			WaveSpeed, WaveStartPosition, WaveEndPosition, PreWaveDuration, PreWaveExplosionDuration,
			FadeoutFireWaveDuration, FadeoutBurningDuration, FadeoutFadingDuration, GrowbackMoonPhaseDuration,
			GrowbackRegrowthStartDuration, GrowbackRegrowthDuration);
		all = PluginMap::ResolveField(scanner, self, TimerNextWaveTimer, kNextWaveTimerRows, static_cast<uint32_t>(std::size(kNextWaveTimerRows))) && all;
		all = PluginMap::ResolveField(scanner, self, TimerWaitingForNextWave, kWaitingForNextWaveRows, static_cast<uint32_t>(std::size(kWaitingForNextWaveRows))) && all;
		all = PluginMap::ResolveField(scanner, self, TimerStopWaves, kStopWavesRows, static_cast<uint32_t>(std::size(kStopWavesRows))) && all;
		all = PluginMap::ResolveField(scanner, self, TimerActor, kTimerActorRows, static_cast<uint32_t>(std::size(kTimerActorRows))) && all;
		return all;
	}
}
