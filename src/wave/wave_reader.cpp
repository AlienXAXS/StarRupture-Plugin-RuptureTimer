#include "wave/wave_reader.h"
#include "wave/wave_model.h"
#include "plugin_helpers.h"
#include "wave/wave_fields.h"

#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace RuptureTimer
{
	namespace
	{
		// Cached per world. The subsystem is found by walking GObjects, which is
		// expensive, so it is resolved once and dropped on world change.
		SDK::UWorld*                     g_cachedWorld      = nullptr;
		SDK::UCrEnviroWaveSubsystem*     g_cachedSubsystem  = nullptr;
		SDK::UCrEnviroWaveTimerSubsystem* g_cachedTimerSubsys = nullptr;
		int                              g_walkFailures     = 0;

		// The longest NextWaveTimer seen in this world, as the total for the
		// progress ring. WaitingDuration itself is not read (see wave_fields.h).
		float                            g_calmLength       = 0.0f;

		// GetCurrentStageSettings returns FCrEnviroWaveSettings by value through
		// ProcessEvent. The SDK's wrapper holds it as a compiled struct; this
		// calls the function with a parameter block of the function's live size
		// (PluginMap::ParamsBlock) and the struct the running build's size, so a
		// build that resizes the struct does not turn the reader off. The struct
		// is the function's only parameter, at offset 0 of the block, and the
		// members are read in place from "ReturnValue". The block is empty when
		// the function, the size or the call is not there.
		PluginMap::ParamsBlock CurrentStageSettings(SDK::UCrEnviroWaveSubsystem* ss)
		{
			SDK::UFunction* function = ss && ss->Class
				? ss->Class->GetFunction("CrEnviroWaveSubsystem", "GetCurrentStageSettings") : nullptr;
			if (!function)
				return PluginMap::ParamsBlock(0, {});

			PluginMap::ParamsBlock call(static_cast<uint32_t>(function->Size),
				{ { "ReturnValue", 0, Fields::WaveSettingsSize.resolvedSize } });
			if (!call)
				return call;

			// Marked native for the call and handed back as it was, the way the
			// SDK's own wrappers call it.
			const auto flags = function->FunctionFlags;
			function->FunctionFlags |= 0x400;
			const bool called = PluginMap::Call(ss, function, call);
			function->FunctionFlags = flags;
			return called ? std::move(call) : PluginMap::ParamsBlock(0, {});
		}

		// ---------------------------------------------------------------------
		// UCrEnviroWaveTimerSubsystem: the fields below live inside the SDK's
		// Pad_30[0x60], so there are no names to use. Their offsets come from the
		// loader's map for the running build (wave_fields.h), taken from the IDA
		// dump (UCrEnviroWaveTimerSubsystem/types.h) on CL-127004.
		//
		// NextWaveTimer is the authority's live countdown: Initialize and
		// OnWaveFinished set it to WaitingDuration, Tick decrements it while
		// bWaitingForNextWave is set and waves are not paused, and the wave
		// starts when it runs out. It is persisted, so it is also right after a
		// save is loaded. The actor's NextTime is not: it is an absolute world
		// time armed at world start, and loading a save then restores the
		// world's clock to the save's play time, leaving NextTime far behind it.
		// ---------------------------------------------------------------------
		//   NextWaveTimer        float, seconds to the next wave
		//   bWaitingForNextWave  bool, the countdown is running
		//   bStopWaves           bool, waves disabled entirely
		//   TimerActor           ACrWaveTimerActor*

		// The calm path runs several times a second, so this must not log per
		// poll. Dedupe on a STABLE key describing the condition -- never on the
		// formatted message, which carries a live clock value and so would be
		// different every time and defeat the guard entirely.
		char g_lastCalmKey[32] = "";

		void ClearCalmLogState() { g_lastCalmKey[0] = '\0'; }

		template <typename... Args>
		void LogCalmOnce(const char* key, const char* fmt, Args... args)
		{
			if (strcmp(key, g_lastCalmKey) == 0) return;
			strncpy_s(g_lastCalmKey, sizeof(g_lastCalmKey), key, _TRUNCATE);

			char buf[256];
			snprintf(buf, sizeof(buf), fmt, args...);
			// Debug, not warn: a stalled schedule is a game-side state that the
			// overlay already reports, not something gone wrong here.
			LOG_DEBUG("WaveReader (calm): %s", buf);
		}

		// Only trust the offsets above if the one field the SDK *does* name lands
		// where the dump says it does. A re-dump that shifts the layout fails
		// this and the plugin quietly falls back to the replicated actor rather
		// than reading rubbish.
		bool TimerSubsystemLayoutMatches(const SDK::UCrEnviroWaveTimerSubsystem* ts,
		                                 const SDK::ACrWaveTimerActor* expectedActor)
		{
			if (!ts || !expectedActor) return false;
			return Fields::ValueOr<SDK::ACrWaveTimerActor*>(Fields::TimerActor, ts, nullptr) == expectedActor;
		}

		// Beyond this the two independent countdowns are treated as contradicting
		// each other rather than as normal replication jitter.
		constexpr float kDisagreementThresholdSeconds = 2.0f;

		constexpr int kMaxWalkFailuresBeforeBackoff = 30;

		SDK::UCrEnviroWaveSubsystem* ResolveSubsystem(SDK::UWorld* world)
		{
			if (g_cachedWorld == world && g_cachedSubsystem)
				return g_cachedSubsystem;

			if (g_cachedWorld != world)
			{
				g_cachedWorld       = world;
				g_cachedSubsystem   = nullptr;
				g_cachedTimerSubsys = nullptr;
				g_walkFailures      = 0;
				g_calmLength        = 0.0f;
			}

			// The subsystem collection is not exposed by the SDK, so GObjects is
			// the only route. InstanceOnly keeps the class default object out.
			auto* hooks = GetHooks();
			if (!hooks || !hooks->ObjectWalker || !hooks->ObjectWalker->IsReady())
				return nullptr;

			if (g_walkFailures >= kMaxWalkFailuresBeforeBackoff)
				return nullptr;

			PluginObjectInfo found[4] = {};
			const int count = hooks->ObjectWalker->FindObjectsByClassNameInto(
				"CrEnviroWaveSubsystem", PluginObjectLookup_InstanceOnly, found, 4);

			if (count <= 0)
			{
				++g_walkFailures;
				return nullptr;
			}

			g_cachedSubsystem = static_cast<SDK::UCrEnviroWaveSubsystem*>(found[0].object);
			g_walkFailures    = 0;
			LOG_DEBUG("WaveReader: resolved UCrEnviroWaveSubsystem at %p (%d instance(s))",
				g_cachedSubsystem, count);
			return g_cachedSubsystem;
		}

		// Server-side only: ShouldCreateSubsystem returns NetMode < 3, so this
		// does not exist on a pure client. Authority builds are the only callers.
		SDK::UCrEnviroWaveTimerSubsystem* ResolveTimerSubsystem()
		{
			if (g_cachedTimerSubsys) return g_cachedTimerSubsys;

			auto* hooks = GetHooks();
			if (!hooks || !hooks->ObjectWalker || !hooks->ObjectWalker->IsReady())
				return nullptr;

			PluginObjectInfo found[4] = {};
			const int count = hooks->ObjectWalker->FindObjectsByClassNameInto(
				"CrEnviroWaveTimerSubsystem", PluginObjectLookup_InstanceOnly, found, 4);
			if (count <= 0) return nullptr;

			g_cachedTimerSubsys = static_cast<SDK::UCrEnviroWaveTimerSubsystem*>(found[0].object);
			LOG_DEBUG("WaveReader: resolved UCrEnviroWaveTimerSubsystem at %p", g_cachedTimerSubsys);
			return g_cachedTimerSubsys;
		}

		SDK::ACrGameStateBase* ResolveGameState(SDK::UWorld* world)
		{
			if (!world || !world->GameState)
				return nullptr;
			if (!world->GameState->IsA(SDK::ACrGameStateBase::StaticClass()))
				return nullptr;
			return static_cast<SDK::ACrGameStateBase*>(world->GameState);
		}

		// Substage byte for whichever stage is running. These are plain named
		// members on the subsystem, so no property lookup is needed.
		uint8_t ReadSubstage(SDK::UCrEnviroWaveSubsystem* ss, WaveStage stage)
		{
			switch (stage)
			{
			case WaveStage::PreWave:  return static_cast<uint8_t>(Fields::ValueOr<SDK::EEnviroWavePreWaveSubstage>(Fields::PreWaveSubstage, ss));
			case WaveStage::Fadeout:  return static_cast<uint8_t>(Fields::ValueOr<SDK::EEnviroWaveFadeoutSubstage>(Fields::FadeoutSubstage, ss));
			case WaveStage::Growback: return static_cast<uint8_t>(Fields::ValueOr<SDK::EEnviroWaveGrowbackSubstage>(Fields::GrowbackSubstage, ss));
			default: return 0;
			}
		}
	}

	void WaveReader::InvalidateCache()
	{
		g_cachedWorld       = nullptr;
		g_cachedSubsystem   = nullptr;
		g_cachedTimerSubsys = nullptr;
		g_walkFailures      = 0;
		g_calmLength        = 0.0f;
		ClearCalmLogState();
	}

	bool WaveReader::Poll(WaveSnapshot& out, WaitReason& outReason)
	{
		out       = WaveSnapshot{};
		outReason = WaitReason::None;

		SDK::UWorld* world = PluginSdkRuntime::GetWorld();
		if (!world)
		{
			outReason = WaitReason::NoWorld;
			return false;
		}

		SDK::ACrGameStateBase* gameState = ResolveGameState(world);
		if (!gameState)
		{
			outReason = WaitReason::NoWorld;
			return false;
		}

		const double now = SDK::UGameplayStatics::GetTimeSeconds(world);

		// ---- Replicated timer actor -------------------------------------------
		// NextPhase encodes the phase currently running (0 = calm/prewave,
		// |1| = Moving, |2| = Fadeout, |3| = Growback) with its sign carrying the
		// wave type (positive Heat, negative Cold). NextTime is the absolute world
		// time that phase ends, not a countdown.
		float timerRemaining = 0.0f;
		bool  timerValid     = false;
		SDK::ACrWaveTimerActor* timer = Fields::ValueOr<SDK::ACrWaveTimerActor*>(Fields::WaveTimerActor, gameState, nullptr);
		const float*   timerNextTime  = Fields::NextTime.In(timer);
		const int32_t* timerNextPhase = Fields::NextPhase.In(timer);
		if (timer && timerNextTime && timerNextPhase)
		{
			out.timerActorResolved = true;

			// Two things are being pinned down here, both only observable by
			// watching a live session:
			//
			//  1. Whether TimeSeconds (what SetPhaseTimer arms against) and
			//     GetServerWorldTimeSeconds actually agree. If they drift, the
			//     countdown reads as overdue while the game is on schedule.
			//  2. The exact moment NextTime/NextPhase move, which is when the
			//     game rearms the schedule -- the transition we have never
			//     managed to catch.
			//
			// Logged only on change, so it is a handful of lines per wave cycle.
			static float s_lastNextTime  = 0.0f;
			static int32_t s_lastPhase   = -999;
			const float   nextTime  = *timerNextTime;
			const int32_t nextPhase = *timerNextPhase;
			if (nextTime != s_lastNextTime || nextPhase != s_lastPhase)
			{
				const double serverTime = gameState->GetServerWorldTimeSeconds();
				LOG_INFO("WaveTimer changed: NextTime %.2f -> %.2f | NextPhase %d -> %d | "
				         "TimeSeconds=%.2f ServerWorldTime=%.2f (delta=%.2f)",
				         s_lastNextTime, nextTime, s_lastPhase, nextPhase,
				         (float)now, (float)serverTime, (float)(now - serverTime));
				s_lastNextTime = nextTime;
				s_lastPhase    = nextPhase;
			}
			out.rawNextTime        = nextTime;
			out.rawNextPhase       = nextPhase;
			out.paused             = Fields::ValueOr<bool>(Fields::Pause, timer);

			// Recorded whatever it works out to -- a negative value is exactly
			// what the diagnostics need to see.
			const float remaining = nextTime - static_cast<float>(now);
			out.rawTimerRemaining = remaining;

			if (IsSaneSeconds(remaining))
			{
				timerRemaining = remaining;
				timerValid     = true;
			}

			if (nextPhase != 0)
				out.waveType = nextPhase < 0 ? WaveType::Cold : WaveType::Heat;
		}

		// ---- Wave subsystem ----------------------------------------------------
		SDK::UCrEnviroWaveSubsystem* ss = ResolveSubsystem(world);
		if (ss)
		{
			out.subsystemResolved = true;
			out.waveInProgress    = ss->IsWaveInProgress();
			out.paused            = out.paused || ss->IsWavePaused();

			if (out.waveInProgress)
			{
				out.stage    = static_cast<WaveStage>(ss->GetCurrentStage());
				out.waveType = static_cast<WaveType>(ss->GetCurrentType());
				out.substage = ReadSubstage(ss, out.stage);

				const float progress = ss->GetCurrentStageProgress();
				if (std::isfinite(progress))
					out.stageProgress = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);

				// GetCurrentStageSettings dereferences the stage pointer with no
				// null check in the game, so it is only ever called behind
				// IsWaveInProgress.
				PluginMap::ParamsBlock current = CurrentStageSettings(ss);
				const uint8_t* settings = current.Param("ReturnValue");

				const float duration = StageDuration(out.stage, settings);
				if (duration > 0.0f)
				{
					out.stageDuration  = duration;
					out.stageRemaining = (1.0f - out.stageProgress) * duration;
					out.stageTimeValid = IsSaneSeconds(out.stageRemaining);
				}

				float substageRemaining = 0.0f;
				if (SubstageRemaining(out.stage, out.substage, out.stageProgress,
				                      settings, &substageRemaining))
				{
					out.substageRemaining = substageRemaining;
					out.substageTimeValid = true;
				}
				out.nextSubstage = NextSubstageOf(out.stage, out.substage);

				if (out.stage == WaveStage::Moving)
				{
					const float x = ss->GetWavePositionX();
					if (std::isfinite(x)) { out.wavePositionX = x; out.positionValid = true; }
				}

				const double since = ss->GetTimeSinceLastWaveStarted();
				if (std::isfinite(since) && since >= 0.0)
					out.timeSinceLastWave = static_cast<float>(since);
			}
		}
		else
		{
			outReason = WaitReason::SubsystemUnresolved;
		}

		out.nextStage = NextStageOf(out.stage);

		// ---- Reconcile the sources --------------------------------------------
		if (!out.waveInProgress)
		{
			// On the authority the timer subsystem holds the live countdown, the
			// one the game itself fires the wave from. A pure client has no such
			// subsystem and falls back to the replicated actor's NextTime.
			float calmTotal     = 0.0f;
			bool  haveCountdown = false;

			SDK::UCrEnviroWaveTimerSubsystem* ts = ResolveTimerSubsystem();
			if (ts)
			{
				if (TimerSubsystemLayoutMatches(ts, timer))
				{
					out.timerSubsysResolved = true;
					out.wavesStopped        = Fields::ValueOr<bool>(Fields::TimerStopWaves, ts);
					out.rawCalmTimer        = Fields::ValueOr<float>(Fields::TimerNextWaveTimer, ts);

					const float countdown = out.rawCalmTimer;
					if (Fields::ValueOr<bool>(Fields::TimerWaitingForNextWave, ts) && IsSaneSeconds(countdown))
					{
						haveCountdown = true;
						if (countdown > g_calmLength)
							g_calmLength = countdown;
						timerRemaining = countdown;
					}
					calmTotal = g_calmLength;
				}
				else
				{
					LogCalmOnce("layout",
					            "UCrEnviroWaveTimerSubsystem layout mismatch: ts=%p TimerActor=%p "
					            "GameState->WaveTimerActor=%p", ts,
					            (const void*)Fields::ValueOr<SDK::ACrWaveTimerActor*>(Fields::TimerActor, ts, nullptr),
					            (const void*)timer);
				}
			}

			// Without the subsystem, the actor's calm deadline is all there is.
			const bool usable = out.timerSubsysResolved
			                  ? haveCountdown
			                  : (timerValid && out.rawNextPhase == 0);

			if (!out.wavesStopped && usable)
			{
				out.nextWaveIn        = timerRemaining;
				out.nextWaveTimeValid = true;
				out.stageRemaining    = timerRemaining;
				out.stageTimeValid    = true;

				// Back to a good state -- let the next failure report itself once.
				ClearCalmLogState();

				// Gives the ring something to sweep through during the calm window.
				if (calmTotal >= timerRemaining && calmTotal > 0.0f)
				{
					out.stageDuration = calmTotal;
					out.stageProgress = 1.0f - (timerRemaining / calmTotal);
				}
			}
			else if (!out.wavesStopped)
			{
				// No wave running and no countdown running: the schedule has
				// stalled. On a client, the actor's deadline passed with nothing
				// having fired.
				out.nextWaveOverdue = out.timerSubsysResolved
				                    ? true
				                    : (out.timerActorResolved && out.rawNextPhase == 0
				                       && out.rawTimerRemaining < 0.0f);

				LogCalmOnce(out.nextWaveOverdue ? "overdue" : "nocountdown",
				            "no usable countdown: NextWaveTimer=%.2f NextPhase=%d NextTime=%.2f "
				            "now=%.2f remaining=%.2f calmTotal=%.2f overdue=%d (actor=%p)",
				            out.rawCalmTimer, out.rawNextPhase, out.rawNextTime, (float)now,
				            out.rawTimerRemaining, calmTotal, (int)out.nextWaveOverdue,
				            (const void*)timer);
			}
		}
		else if (out.stageTimeValid && timerValid && out.rawNextPhase != 0 &&
		         out.stage != WaveStage::PreWave)
		{
			// Both sources describe the same phase here (the timer has no PreWave
			// value). Disagreement means one of them is wrong, so prefer the
			// replicated one and flag it rather than quietly picking a winner.
			if (std::fabs(out.stageRemaining - timerRemaining) > kDisagreementThresholdSeconds)
			{
				out.sourcesDisagree = true;
				out.stageRemaining  = timerRemaining;
			}
		}

		if (!out.subsystemResolved && !out.timerActorResolved)
		{
			outReason = WaitReason::SubsystemUnresolved;
			return false;
		}

		return true;
	}
}
