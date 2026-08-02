#pragma once

// Debug-build tooling. Investigation scaffolding rather than a feature, so it is
// compiled out of Release entirely -- a 100 MB GObjects walk behind a button is
// not something to ship. The functions stay declared and become no-op stubs, so
// call sites need no guards of their own.
#if defined(_DEBUG) && !defined(MODLOADER_SERVER_BUILD)
#define RUPTURETIMER_DEBUG_TOOLS 1
#endif

namespace RuptureTimer
{
	// Investigation aid: walks GObjects for wave-related objects and logs every
	// reflected property with its offset and current value.
	//
	// Exists because no native code decides when a wave starts -- StartWave has
	// no caller outside its own exec thunk -- so the schedule appears to live in
	// Blueprint, where the only way to find the real countdown is to look.
	//
	// No-ops on server builds.
	namespace ObjectDumper
	{
		// Called from the UI (render thread). Only raises a flag; the walk itself
		// is expensive and touches UObjects, so it runs on the game thread.
		void RequestDump();

		// Game thread. Runs a pending dump, if any.
		void PumpPendingDump();

		bool IsDumpPending();
	}
}
