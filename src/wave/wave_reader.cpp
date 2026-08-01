#include "wave/wave_reader.h"
#include "wave/wave_model.h"
#include "plugin_helpers.h"

#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

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

		// ---------------------------------------------------------------------
		// UCrEnviroWaveTimerSubsystem: the fields below live inside the SDK's
		// Pad_30[0x60], so there are no names to use and these are raw offsets
		// taken from the IDA dump (UCrEnviroWaveTimerSubsystem/types.h).
		//
		// NOTE on NextWaveTimer: despite the name it is NOT a running countdown.
		// Nothing in the binary ever decrements it -- it is only assigned in
		// Initialize and OnWaveFinished (both `= WaitingDuration`) and then
		// persisted. So it is the configured length of the calm window, useful
		// as the total for a progress ring, and useless as "time remaining".
		// The live countdown is ACrWaveTimerActor::NextTime.
		// ---------------------------------------------------------------------
		constexpr int kOff_WaitingDuration     = 0x78;   // float, configured calm length
		constexpr int kOff_NextWaveTimer       = 0x7C;   // float, == WaitingDuration (see above)
		constexpr int kOff_bWaitingForNextWave = 0x80;   // bool, 1 between waves, 0 during one
		constexpr int kOff_bStopWaves          = 0x83;   // bool, waves disabled entirely
		constexpr int kOff_TimerActor          = 0x90;   // ACrWaveTimerActor*

		template <typename T>
		const T& AtOffset(const void* base, int offset)
		{
			return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(base) + offset);
		}

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
			// Debug, not warn: a stale save timer is a known game-side limitation
			// that the overlay already reports, not something gone wrong here.
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
			return AtOffset<const SDK::ACrWaveTimerActor*>(ts, kOff_TimerActor) == expectedActor;
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
			case WaveStage::PreWave:  return static_cast<uint8_t>(ss->CurrentPreWaveSubstage);
			case WaveStage::Fadeout:  return static_cast<uint8_t>(ss->CurrentFadeoutSubstage);
			case WaveStage::Growback: return static_cast<uint8_t>(ss->CurrentGrowbackSubstage);
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
		ClearCalmLogState();
	}

	bool WaveReader::Poll(WaveSnapshot& out, WaitReason& outReason)
	{
		out       = WaveSnapshot{};
		outReason = WaitReason::None;

		SDK::UWorld* world = SDK::UWorld::GetWorld();
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
		if (SDK::ACrWaveTimerActor* timer = gameState->WaveTimerActor)
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
			if (timer->NextTime != s_lastNextTime || timer->NextPhase != s_lastPhase)
			{
				const double serverTime = gameState->GetServerWorldTimeSeconds();
				LOG_INFO("WaveTimer changed: NextTime %.2f -> %.2f | NextPhase %d -> %d | "
				         "TimeSeconds=%.2f ServerWorldTime=%.2f (delta=%.2f)",
				         s_lastNextTime, timer->NextTime, s_lastPhase, timer->NextPhase,
				         (float)now, (float)serverTime, (float)(now - serverTime));
				s_lastNextTime = timer->NextTime;
				s_lastPhase    = timer->NextPhase;
			}
			out.rawNextTime        = timer->NextTime;
			out.rawNextPhase       = timer->NextPhase;
			out.paused             = timer->bPause;

			// Recorded whatever it works out to -- a negative value is exactly
			// what the diagnostics need to see, and is how the stale-save case
			// is identified below.
			const float remaining = timer->NextTime - static_cast<float>(now);
			out.rawTimerRemaining = remaining;

			if (IsSaneSeconds(remaining))
			{
				timerRemaining = remaining;
				timerValid     = true;
			}

			if (timer->NextPhase != 0)
				out.waveType = timer->NextPhase < 0 ? WaveType::Cold : WaveType::Heat;
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
				const SDK::FCrEnviroWaveSettings settings = ss->GetCurrentStageSettings();

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
			// The timer subsystem supplies whether waves are running at all, and
			// how long a calm window lasts -- but not how much of it is left.
			// The only live countdown is the replicated actor's NextTime.
			float calmTotal = 0.0f;

			SDK::UCrEnviroWaveTimerSubsystem* ts = ResolveTimerSubsystem();
			if (ts)
			{
				if (TimerSubsystemLayoutMatches(ts, gameState->WaveTimerActor))
				{
					out.timerSubsysResolved = true;
					out.wavesStopped        = AtOffset<bool>(ts, kOff_bStopWaves);
					out.rawCalmTimer        = AtOffset<float>(ts, kOff_NextWaveTimer);

					const float configured = AtOffset<float>(ts, kOff_WaitingDuration);
					if (IsSaneSeconds(configured) && configured > 0.0f)
						calmTotal = configured;
				}
				else
				{
					LogCalmOnce("layout",
					            "UCrEnviroWaveTimerSubsystem layout mismatch: ts=%p +0x90=%p "
					            "GameState->WaveTimerActor=%p", ts,
					            AtOffset<const void*>(ts, kOff_TimerActor),
					            (const void*)gameState->WaveTimerActor);
				}
			}

			// Is NextTime something this session actually armed?
			//
			// SetPhaseTimer always writes `NextTime = WaitingDuration + TimeSeconds`
			// for phase 0, so a legitimately armed calm timer can never be more
			// than WaitingDuration away. At world start that makes NextTime equal
			// WaitingDuration exactly, and the countdown to the session's first
			// wave is real for that whole window.
			//
			// Past it, if no wave started, nothing rearms NextTime and it slides
			// negative -- the schedule has stalled and there is nothing to show.
			const float kArmedEpsilon = 1.0f;
			bool plausible = timerValid;
			if (plausible && calmTotal > 0.0f && timerRemaining > calmTotal + kArmedEpsilon)
				plausible = false;

			if (!out.wavesStopped && plausible && out.rawNextPhase == 0)
			{
				out.nextWaveIn        = timerRemaining;
				out.nextWaveTimeValid = true;
				out.stageRemaining    = timerRemaining;
				out.stageTimeValid    = true;

				// Back to a good state -- let the next failure report itself once.
				ClearCalmLogState();

				// Gives the ring something to sweep through during the calm window.
				if (calmTotal >= timerRemaining)
				{
					out.stageDuration = calmTotal;
					out.stageProgress = 1.0f - (timerRemaining / calmTotal);
				}
			}
			else if (!out.wavesStopped)
			{
				const bool haveActor = out.timerActorResolved && out.rawNextPhase == 0;

				// Too far away to be this session's calm window -- a restored
				// absolute timestamp.
				out.nextWaveTimeStale = haveActor && calmTotal > 0.0f
				                     && out.rawTimerRemaining > calmTotal + kArmedEpsilon;

				// Already elapsed with nothing having fired.
				out.nextWaveOverdue = haveActor && !out.nextWaveTimeStale
				                   && out.rawTimerRemaining < 0.0f;

				LogCalmOnce(out.nextWaveTimeStale ? "stalesave"
				          : out.nextWaveOverdue   ? "overdue" : "nocountdown",
				            "no usable countdown: NextPhase=%d NextTime=%.2f now=%.2f "
				            "remaining=%.2f calmTotal=%.2f stale=%d (actor=%p)",
				            out.rawNextPhase, out.rawNextTime, (float)now,
				            out.rawTimerRemaining, calmTotal, (int)out.nextWaveOverdue,
				            (const void*)gameState->WaveTimerActor);
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
