#include "plugin.h"
#include "plugin_helpers.h"
#include "config/plugin_config.h"
#include "net/wave_net.h"
#include "ui/overlay_widget.h"
#include "ui/diagnostics_window.h"
#include "debug/object_dumper.h"
#include "debug/wave_actions.h"

#include "Engine_classes.hpp"

#include <cstring>
#include <string>

using namespace RuptureTimer;

static IPluginSelf* g_self = nullptr;

IPluginSelf* GetSelf() { return g_self; }

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

// The loader refuses to load a DLL whose target doesn't match its own build.
#if defined(MODLOADER_SERVER_BUILD)
#define PLUGIN_TARGET_THIS PLUGIN_TARGET_SERVER
static const char* kBuildFlavour = "server";
#else
#define PLUGIN_TARGET_THIS PLUGIN_TARGET_CLIENT
static const char* kBuildFlavour = "client";
#endif

// The name is also the network routing key — the client and server builds must
// agree on it exactly or packets are silently dropped.
static PluginInfo s_pluginInfo = {
	"AxRuptureTimer",
	MODLOADER_BUILD_TAG,
	"AlienX",
	"Shows the current Rupture Wave state, the time until the next state, and what comes next.",
	PLUGIN_INTERFACE_VERSION,
	PLUGIN_TARGET_THIS
};

static bool g_isActive       = false;
static bool g_tickRegistered = false;
static char g_boundOverlayKey[32] = "";

static void OnTick(float deltaSeconds)
{
	if (!g_isActive) return;
	WaveNet::Tick(deltaSeconds);

	// Game thread: walking GObjects and reading UObject reflection cannot be
	// done from the render thread where the button lives.
	ObjectDumper::PumpPendingDump();
	WaveActions::PumpPendingAction();
}

static void OnOverlayKey(EModKey, EModKeyEvent) { Overlay::Toggle(); }

// Rebinds only when the configured combo actually changed, so a config edit to
// an unrelated key doesn't churn the registry.
static void BindKeys()
{
	auto* hooks = GetHooks();
	if (!hooks || !hooks->Input) return;

	const char* wanted = Config::Get().toggleKey;
	if (!wanted || !wanted[0]) return;
	if (g_boundOverlayKey[0] && strcmp(g_boundOverlayKey, wanted) == 0) return;

	if (g_boundOverlayKey[0])
		hooks->Input->UnregisterKeybindByName(g_boundOverlayKey, EModKeyEvent::Pressed, &OnOverlayKey);

	hooks->Input->RegisterKeybindByName(wanted, EModKeyEvent::Pressed, &OnOverlayKey);
	strncpy_s(g_boundOverlayKey, sizeof(g_boundOverlayKey), wanted, _TRUNCATE);
	LOG_DEBUG("Overlay toggle bound to '%s'", g_boundOverlayKey);
}

static void UnbindKeys()
{
	auto* hooks = GetHooks();
	if (hooks && hooks->Input && g_boundOverlayKey[0])
		hooks->Input->UnregisterKeybindByName(g_boundOverlayKey, EModKeyEvent::Pressed, &OnOverlayKey);
	g_boundOverlayKey[0] = '\0';
}

static void OnConfigChanged(const char* section, const char* key, const char* newValue)
{
	// The reported value has to be passed through: the ModLoader notifies live
	// but flushes to disk separately, so re-reading the INI here would pick up
	// the previous value and leave the UI an edit behind.
	Config::Reload(section, key, newValue);
	LOG_DEBUG("Config changed: [%s] %s = %s", section ? section : "?", key ? key : "?",
		newValue ? newValue : "?");

	BindKeys();
	// The overlay only exists while a session does; outside one there is nothing
	// to re-apply it to.
	if (g_isActive) Overlay::ApplySettings();
	DiagnosticsWindow::ApplySettings();
}

static void Activate()
{
	if (g_isActive) return;

	WaveNet::ResetSession();

	auto* hooks = GetHooks();
	if (hooks && hooks->Engine && !g_tickRegistered)
	{
		hooks->Engine->RegisterOnTick(&OnTick);
		g_tickRegistered = true;
	}

	Overlay::ApplySettings();
	BindKeys();

	g_isActive = true;
	LOG_INFO("RuptureTimer active");
}

static void Deactivate()
{
	if (!g_isActive) return;
	g_isActive = false;

	// Leaving the world means there is nothing to report — take the overlay down
	// rather than leaving a "Detecting session..." card on the main menu.
	Overlay::Unregister();
	WaveNet::ResetSession();
	LOG_INFO("RuptureTimer inactive");
}

static void OnWorldBeginPlay(SDK::UWorld* world, const char* worldName)
{
	LOG_DEBUG("OnWorldBeginPlay: world=%p name=%s", world, worldName ? worldName : "(null)");
	if (worldName && strcmp(worldName, "ChimeraMain") == 0)
		Activate();
}

static void OnWorldEndPlay(SDK::UWorld* world, const char* worldName)
{
	LOG_DEBUG("OnWorldEndPlay: world=%p name=%s", world, worldName ? worldName : "(null)");
	Deactivate();
}

extern "C" {

	__declspec(dllexport) PluginInfo* GetPluginInfo()
	{
		return &s_pluginInfo;
	}

	__declspec(dllexport) bool PluginInit(IPluginSelf* self)
	{
		try
		{
			g_self = self;

			LOG_INFO("RuptureTimer %s initializing (%s build)", self->version, kBuildFlavour);

			Config::Initialize(self);

			if (!Config::Get().enabled)
			{
				LOG_WARN("Plugin is disabled in config file");
				return true;
			}

			WaveNet::Initialize();

			auto* hooks = self->hooks;
			if (!hooks || !hooks->World)
			{
				LOG_ERROR("World hooks unavailable — cannot track the wave");
				return false;
			}

			hooks->World->RegisterOnAnyWorldBeginPlay(&OnWorldBeginPlay);
			hooks->World->RegisterOnBeforeWorldEndPlay(&OnWorldEndPlay);

			// Nothing is hooked for joining players here: a snapshot sent at
			// player-join is dropped, because the client has not reported itself
			// to the loader yet. WaveNet drives that off the client-ready
			// callback instead, which fires once that is actually true.

			if (hooks->UI)
				hooks->UI->RegisterOnConfigChanged(self, &OnConfigChanged);

			// Registered up front (hidden by default) so the toggle key works
			// before a world exists — unlike the overlay, which is session-bound.
			DiagnosticsWindow::ApplySettings();
			BindKeys();

			// Hot-reload: the world may already be up.
			if (SDK::UWorld* currentWorld = SDK::UWorld::GetWorld())
			{
				const std::string worldName = currentWorld->GetName();
				if (worldName == "ChimeraMain")
				{
					LOG_INFO("ChimeraMain already running — activating now");
					Activate();
				}
			}

			LOG_INFO("RuptureTimer initialized");
			return true;
		}
		catch (const std::exception& e)
		{
			LOG_ERROR("PluginInit: caught exception: %s", e.what());
			return false;
		}
		catch (...)
		{
			LOG_ERROR("PluginInit: caught unknown exception");
			return false;
		}
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("RuptureTimer shutting down...");

		g_isActive = false;

		if (g_self && g_self->hooks)
		{
			auto* hooks = g_self->hooks;

			if (hooks->Engine && g_tickRegistered)
			{
				hooks->Engine->UnregisterOnTick(&OnTick);
				g_tickRegistered = false;
			}
			if (hooks->World)
			{
				hooks->World->UnregisterOnAnyWorldBeginPlay(&OnWorldBeginPlay);
				hooks->World->UnregisterOnBeforeWorldEndPlay(&OnWorldEndPlay);
			}
			if (hooks->UI)
				hooks->UI->UnregisterOnConfigChanged(g_self, &OnConfigChanged);
		}

		UnbindKeys();
		DiagnosticsWindow::Unregister();
		Overlay::Unregister();
		WaveNet::Shutdown();

		g_self = nullptr;
	}

} // extern "C"
