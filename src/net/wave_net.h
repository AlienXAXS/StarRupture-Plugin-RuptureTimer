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

		// Loader readiness (v56). A peer that is not ready is not a valid packet
		// target — sends to it are dropped, not queued — so these are the
		// difference between "the link is quiet" and "there is no link yet".
		bool     serverLinkReady = false;  // client: authority acknowledged us
		uint32_t readyClients    = 0;      // authority: clients that can receive

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

		// Server: push a fresh snapshot to one client. Only meaningful once that
		// client is ready — before then the loader drops the packet — so this is
		// driven by the client-ready callback, not by player-joined.
		void SendSnapshotTo(void* playerController);

		// Render-thread safe copy of what to draw.
		DisplayState GetDisplayState();
	}
}
