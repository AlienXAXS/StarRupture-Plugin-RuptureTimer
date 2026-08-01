#pragma once

#include "plugin_interface.h"

namespace RuptureTimer
{
	// Everything the render path needs, read from the INI once and refreshed on
	// the config-changed callback. The overlay draws on the render thread and
	// must never touch the config file.
	struct Settings
	{
		// General
		bool  enabled            = true;
		bool  showOverlay        = true;
		char  toggleKey[32]      = "F7";

		// Display
		bool  showStageTimeline  = true;
		bool  showProgressRing   = true;
		bool  showNextState      = true;
		bool  showSubstage       = true;
		bool  showWavePosition   = false;
		bool  showNetworkStatus  = true;
		bool  showTimeSinceWave  = false;
		bool  compactMode        = false;
		bool  lockWindow         = false;
		bool  accentByWaveType   = true;
		float scale              = 1.0f;
		float opacity            = 0.88f;
		int   accentPreset       = 0;

		// Network
		float broadcastInterval  = 1.0f;
		float staleAfterSeconds  = 5.0f;
		bool  countdownInterp    = true;

		// Advanced
		bool  verboseLogging     = false;
		bool  showDiagnostics    = false;
	};

	namespace Config
	{
		// Creates the INI from the schema if missing, then loads it.
		void Initialize(IPluginSelf* self);

		// Re-read every value from the INI.
		//
		// The optional triplet is the edit reported by RegisterOnConfigChanged.
		// The ModLoader notifies live but flushes to disk separately, so during
		// that callback the file still holds the PREVIOUS value -- reading it
		// blind leaves the UI one edit behind. Pass the reported section/key/
		// value and it wins over what is on disk for that one key.
		void Reload(const char* liveSection = nullptr,
		            const char* liveKey     = nullptr,
		            const char* liveValue   = nullptr);

		// Safe to call from the render thread while Reload runs on the game
		// thread: the settings are double-buffered and swapped atomically, so a
		// reader always sees one consistent generation.
		const Settings& Get();

		// Write a boolean back to the INI and refresh the cache. Used when a
		// keybind toggles a window, so the ModLoader config checkbox stays in
		// sync and a later config edit doesn't snap the window back.
		void PersistBool(const char* section, const char* key, bool value);

		extern const ConfigSchema SCHEMA;
	}
}
