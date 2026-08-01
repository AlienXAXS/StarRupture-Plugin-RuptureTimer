#pragma once

namespace RuptureTimer
{
	// The always-on wave readout. All functions are no-ops on server builds.
	namespace Overlay
	{
		void Register();
		void Unregister();

		// Re-applies window hints and visibility from the current settings.
		// Cheap; call after a config change.
		void ApplySettings();

		void Toggle();
		bool IsVisible();
	}
}
