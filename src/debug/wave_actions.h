#pragma once

#include "debug/object_dumper.h"   // for RUPTURETIMER_DEBUG_TOOLS

namespace RuptureTimer
{
	// Fires the game's own debug wave RPCs on ACrPlayerControllerBase, so a wave
	// can be triggered on demand instead of waiting out a 40 minute schedule.
	//
	// These are Server RPCs, so invoking them on the local controller routes to
	// the server -- which is what makes them useful for testing a dedicated
	// server from a connected client.
	//
	// Debug builds only; no-op stubs otherwise.
	namespace WaveActions
	{
		enum class Action
		{
			None = 0,
			StartHeatWave,     // ServerDebugStartResumeDefaultHeatWave
			SkipToNextStage,   // ServerDebugSkipToNextWaveStage
			PauseWave,         // ServerDebugPauseWave
			CancelWave,        // ServerDebugCancelWave
		};

		// UI thread: only records the request.
		void Request(Action action);

		// Game thread: runs a pending request. ProcessEvent is game-thread only.
		void PumpPendingAction();

		bool IsAvailable();
	}
}
