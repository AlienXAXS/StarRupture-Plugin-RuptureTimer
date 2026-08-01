#pragma once

namespace RuptureTimer
{
	// Raw snapshot inspector, for verifying the wave model against a live session.
	//
	// This is a widget rather than a ModLoader config-tab panel on purpose: an
	// open panel counts towards the loader's exclusive input capture, which
	// freezes the player out of the game entirely. A widget holding a cooperative
	// passthrough token stays clickable and scrollable while you keep playing —
	// which is the whole point of watching the wave numbers live.
	//
	// No-ops on server builds.
	namespace DiagnosticsWindow
	{
		void Register();
		void Unregister();

		// Re-applies visibility from the current settings.
		void ApplySettings();

		void Toggle();
		bool IsVisible();
	}
}
