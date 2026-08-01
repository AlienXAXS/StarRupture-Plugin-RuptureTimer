#include "config/plugin_config.h"
#include "plugin_helpers.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace RuptureTimer
{
	namespace
	{
		IPluginSelf* s_self = nullptr;

		// Reload writes the inactive slot then flips the index, so the render
		// thread never observes a half-updated Settings.
		Settings              s_slots[2]  = {};
		std::atomic<unsigned> s_activeSlot{ 0 };

		// Entries with rangeMax > rangeMin render as sliders in the ModLoader
		// config UI; the rest render as plain inputs / checkboxes / rebind rows.
		const ConfigEntry kEntries[] = {
			{ "General", "Enabled",             ConfigValueType::Boolean, "true",  "Enable the RuptureTimer plugin",                         0.0f, 0.0f },
			{ "General", "ShowOverlay",         ConfigValueType::Boolean, "true",  "Show the on-screen wave overlay",                        0.0f, 0.0f },
			{ "General", "ToggleKey",           ConfigValueType::Keybind, "F7",    "Key that shows/hides the overlay",                       0.0f, 0.0f },

			{ "Display", "ShowStageTimeline",   ConfigValueType::Boolean, "true",  "Show the Pre-Wave / Moving / Fadeout / Growback track",  0.0f, 0.0f },
			{ "Display", "ShowProgressRing",    ConfigValueType::Boolean, "true",  "Show the countdown ring around the timer",               0.0f, 0.0f },
			{ "Display", "ShowNextState",       ConfigValueType::Boolean, "true",  "Show which state comes next",                            0.0f, 0.0f },
			{ "Display", "ShowSubstage",        ConfigValueType::Boolean, "true",  "Show the current substage (Burning, Regrowth, ...)",     0.0f, 0.0f },
			{ "Display", "ShowWavePosition",    ConfigValueType::Boolean, "false", "Show the wave's map position while it is moving",        0.0f, 0.0f },
			{ "Display", "ShowNetworkStatus",   ConfigValueType::Boolean, "true",  "Show the session role and data freshness indicator",     0.0f, 0.0f },
			{ "Display", "ShowTimeSinceWave",   ConfigValueType::Boolean, "false", "Show how long ago the current wave started",             0.0f, 0.0f },
			{ "Display", "CompactMode",         ConfigValueType::Boolean, "false", "Shrink the overlay to just the countdown",               0.0f, 0.0f },
			{ "Display", "LockWindow",          ConfigValueType::Boolean, "false", "Pin the overlay in place so it cannot be dragged",       0.0f, 0.0f },
			{ "Display", "AccentByWaveType",    ConfigValueType::Boolean, "true",  "Tint the overlay by wave type (ember for Heat, ice for Cold)", 0.0f, 0.0f },
			{ "Display", "Scale",               ConfigValueType::Float,   "1.0",   "Overlay size multiplier",                                0.6f, 2.0f },
			{ "Display", "Opacity",             ConfigValueType::Float,   "0.88",  "Overlay background opacity",                             0.15f, 1.0f },
			{ "Display", "AccentPreset",        ConfigValueType::Integer, "0",     "Colour theme: 0 Signal, 1 Amber, 2 Violet, 3 Mono",      0.0f, 3.0f },

			{ "Network", "BroadcastIntervalSeconds", ConfigValueType::Float, "1.0", "How often the server sends a wave snapshot (seconds)",  0.25f, 10.0f },
			{ "Network", "StaleAfterSeconds",  ConfigValueType::Float,   "5.0",   "Mark the data stale after this long without a packet",   2.0f, 30.0f },
			{ "Network", "CountdownInterpolation", ConfigValueType::Boolean, "true", "Tick the countdown locally between server packets",   0.0f, 0.0f },

			{ "Advanced", "VerboseLogging",     ConfigValueType::Boolean, "false", "Log wave state transitions and packet traffic",          0.0f, 0.0f },
			{ "Advanced", "ShowDiagnosticsWindow", ConfigValueType::Boolean, "false", "Show the raw wave diagnostics window",              0.0f, 0.0f },
		};

		// The one key currently being edited, valid only for the duration of a
		// Reload call. See the header for why the INI cannot be trusted for it.
		const char* s_liveSection = nullptr;
		const char* s_liveKey     = nullptr;
		const char* s_liveValue   = nullptr;

		bool IsLiveKey(const char* section, const char* key)
		{
			return s_liveValue && s_liveSection && s_liveKey
			    && _stricmp(s_liveSection, section) == 0
			    && _stricmp(s_liveKey, key) == 0;
		}

		bool ParseBool(const char* v, bool def)
		{
			if (!v || !*v) return def;
			return *v == '1' || *v == 't' || *v == 'T' || *v == 'y' || *v == 'Y';
		}

		float Clamp(float v, float lo, float hi)
		{
			if (!(v >= lo)) return lo;   // also catches NaN
			return v > hi ? hi : v;
		}
	}

	const ConfigSchema Config::SCHEMA = { kEntries, static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0])) };

	void Config::Initialize(IPluginSelf* self)
	{
		s_self = self;
		if (!s_self) return;

		s_self->config->InitializeFromSchema(s_self, &SCHEMA);
		s_self->config->ValidateConfig(s_self, &SCHEMA);
		Reload();
	}

	void Config::Reload(const char* liveSection, const char* liveKey, const char* liveValue)
	{
		if (!s_self) return;

		s_liveSection = liveSection;
		s_liveKey     = liveKey;
		s_liveValue   = liveValue;

		Settings s;
		auto* c = s_self->config;

		// Every read goes through these so the live edit is honoured uniformly,
		// whichever key it happened to be.
		auto readBool = [&](const char* sec, const char* key, bool def) {
			return IsLiveKey(sec, key) ? ParseBool(s_liveValue, def)
			                           : c->ReadBool(s_self, sec, key, def);
		};
		auto readFloat = [&](const char* sec, const char* key, float def, float lo, float hi) {
			const float v = IsLiveKey(sec, key) ? static_cast<float>(atof(s_liveValue))
			                                    : c->ReadFloat(s_self, sec, key, def);
			return Clamp(v, lo, hi);
		};
		auto readInt = [&](const char* sec, const char* key, int def) {
			return IsLiveKey(sec, key) ? atoi(s_liveValue) : c->ReadInt(s_self, sec, key, def);
		};
		auto readString = [&](const char* sec, const char* key, char* out, size_t outLen, const char* def) {
			if (IsLiveKey(sec, key)) strncpy_s(out, outLen, s_liveValue, _TRUNCATE);
			else                     c->ReadString(s_self, sec, key, out, static_cast<int>(outLen), def);
		};

		s.enabled           = readBool("General", "Enabled", true);
		s.showOverlay       = readBool("General", "ShowOverlay", true);
		readString("General", "ToggleKey", s.toggleKey, sizeof(s.toggleKey), "F7");

		s.showStageTimeline = readBool("Display", "ShowStageTimeline", true);
		s.showProgressRing  = readBool("Display", "ShowProgressRing", true);
		s.showNextState     = readBool("Display", "ShowNextState", true);
		s.showSubstage      = readBool("Display", "ShowSubstage", true);
		s.showWavePosition  = readBool("Display", "ShowWavePosition", false);
		s.showNetworkStatus = readBool("Display", "ShowNetworkStatus", true);
		s.showTimeSinceWave = readBool("Display", "ShowTimeSinceWave", false);
		s.compactMode       = readBool("Display", "CompactMode", false);
		s.lockWindow        = readBool("Display", "LockWindow", false);
		s.accentByWaveType  = readBool("Display", "AccentByWaveType", true);
		s.scale             = readFloat("Display", "Scale", 1.0f, 0.6f, 2.0f);
		s.opacity           = readFloat("Display", "Opacity", 0.88f, 0.15f, 1.0f);
		s.accentPreset      = readInt("Display", "AccentPreset", 0);
		if (s.accentPreset < 0 || s.accentPreset > 3) s.accentPreset = 0;

		s.broadcastInterval = readFloat("Network", "BroadcastIntervalSeconds", 1.0f, 0.25f, 10.0f);
		s.staleAfterSeconds = readFloat("Network", "StaleAfterSeconds", 5.0f, 2.0f, 30.0f);
		s.countdownInterp   = readBool("Network", "CountdownInterpolation", true);

		s.verboseLogging    = readBool("Advanced", "VerboseLogging", false);
		s.showDiagnostics   = readBool("Advanced", "ShowDiagnosticsWindow", false);

		s_liveSection = s_liveKey = s_liveValue = nullptr;

		const unsigned next = 1u - s_activeSlot.load(std::memory_order_relaxed);
		s_slots[next] = s;
		s_activeSlot.store(next, std::memory_order_release);
	}

	const Settings& Config::Get()
	{
		return s_slots[s_activeSlot.load(std::memory_order_acquire)];
	}

	void Config::PersistBool(const char* section, const char* key, bool value)
	{
		if (!s_self) return;
		s_self->config->WriteBool(s_self, section, key, value);
		Reload();
	}
}
