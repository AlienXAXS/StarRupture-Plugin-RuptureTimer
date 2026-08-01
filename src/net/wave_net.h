#pragma once

#include "wave/wave_types.h"

namespace RuptureTimer
{
	// What the UI should draw right now, assembled from either the local
	// authority read or the last packet off the wire.
	struct DisplayState
	{
		WaveSnapshot snapshot;
		SessionRole  role       = SessionRole::Unresolved;
		WaitReason   waitReason = WaitReason::DetectingSession;

		bool  hasData     = false;   // false => draw the waiting card, no numbers
		bool  stale       = false;   // had data, but the link has gone quiet
		float dataAgeSecs = 0.0f;

		// Diagnostics
		uint32_t sequence        = 0;
		uint32_t packetsReceived = 0;
		uint32_t packetsSent     = 0;
	};

	namespace WaveNet
	{
		// Registers the packet handlers appropriate to this build. Safe to call
		// when hooks->Network is null (generic builds); everything then no-ops.
		void Initialize();
		void Shutdown();

		// Called when the world changes, to clear per-session state.
		void ResetSession();

		// Game-thread tick. Polls the wave on authority builds, broadcasts on the
		// configured interval (and immediately on a state transition), and ages
		// out client-side data.
		void Tick(float deltaSeconds);

		// Server: push a fresh snapshot to one player that just joined.
		void SendSnapshotTo(void* playerController);

		// Render-thread safe copy of what to draw.
		DisplayState GetDisplayState();
	}
}
