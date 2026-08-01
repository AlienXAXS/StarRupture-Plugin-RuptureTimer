#pragma once

#include "wave/wave_types.h"

namespace RuptureTimer
{
	// Authority-side reader. Pulls the wave state out of the running game and
	// normalises it into a WaveSnapshot. Game thread only — every read goes
	// through ProcessEvent.
	namespace WaveReader
	{
		// Drop cached object pointers. Call on world begin/end play.
		void InvalidateCache();

		// Fill `out` from the live game. Returns false when there is nothing to
		// read yet (no world, no game state); `outReason` then says why.
		// A true return with subsystemResolved == false still produces a usable
		// snapshot from the replicated timer actor alone.
		bool Poll(WaveSnapshot& out, WaitReason& outReason);
	}
}
