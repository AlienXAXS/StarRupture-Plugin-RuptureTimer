#include "ui/diagnostics_window.h"

#if defined(MODLOADER_SERVER_BUILD)

namespace RuptureTimer
{
	void DiagnosticsWindow::Register() {}
	void DiagnosticsWindow::Unregister() {}
	void DiagnosticsWindow::ApplySettings() {}
	void DiagnosticsWindow::Toggle() {}
	bool DiagnosticsWindow::IsVisible() { return false; }
}

#else

#include "ui/overlay_widget.h"
#include "debug/object_dumper.h"
#include "debug/wave_actions.h"
#include "net/wave_net.h"
#include "config/plugin_config.h"
#include "plugin_helpers.h"

#include <cstdarg>
#include <cstdio>

namespace RuptureTimer
{
	namespace
	{
		WidgetHandle      g_widget           = nullptr;
		PluginWindowHints g_hints            = {};
		bool              g_visible          = false;
		void*             g_passthroughToken = nullptr;

		// Cooperative passthrough: ImGui gets the mouse only while the cursor is
		// over an ImGui window, so camera look and movement still reach the game.
		// Tokens are refcounted loader-wide, so holding one alongside the
		// overlay's is fine.
		void UpdatePassthrough()
		{
			auto* hooks = GetHooks();
			if (!hooks || !hooks->UI) return;

			if (g_visible && !g_passthroughToken)
				g_passthroughToken = hooks->UI->AcquireInputPassthrough();
			else if (!g_visible && g_passthroughToken)
			{
				hooks->UI->ReleaseInputPassthrough(g_passthroughToken);
				g_passthroughToken = nullptr;
			}
		}

		void Row(IModLoaderImGui* ui, const char* label, const char* value)
		{
			ui->TableNextColumn();
			ui->TextDisabled(label);
			ui->TableNextColumn();
			ui->Text(value);
		}

		void RowF(IModLoaderImGui* ui, const char* label, const char* fmt, ...)
		{
			char buf[160];
			va_list args;
			va_start(args, fmt);
			vsnprintf(buf, sizeof(buf), fmt, args);
			va_end(args);
			Row(ui, label, buf);
		}

		const char* YesNo(bool b) { return b ? "yes" : "no"; }

		void Render(IModLoaderImGui* ui)
		{
			const DisplayState  st  = WaveNet::GetDisplayState();
			const WaveSnapshot& s   = st.snapshot;
			const Settings&     cfg = Config::Get();

			ui->SeparatorText("Session");
			if (ui->BeginTable("rt_session", 2, 0))
			{
				Row(ui, "Role", RoleName(st.role));
				Row(ui, "Has data", YesNo(st.hasData));
				Row(ui, "Stale", YesNo(st.stale));
				RowF(ui, "Data age", "%.2f s", st.dataAgeSecs);
				Row(ui, "Waiting on", st.waitReason == WaitReason::None ? "-" : WaitReasonText(st.waitReason));

				// A peer that is not ready is not a valid packet target, so this is
				// the first thing to check when nothing is arriving.
				if (st.role == SessionRole::RemoteClient)
					Row(ui, "Server link", st.serverLinkReady ? "ready" : "handshaking");
				else if (st.role == SessionRole::ListenHost || st.role == SessionRole::DedicatedServer)
					RowF(ui, "Ready clients", "%u", st.readyClients);
				ui->EndTable();
			}

			ui->SeparatorText("Wave state");
			if (ui->BeginTable("rt_state", 2, 0))
			{
				Row(ui, "Stage", StageName(s.stage));
				Row(ui, "Next stage", StageName(s.nextStage));
				Row(ui, "Wave type", WaveTypeName(s.waveType));
				RowF(ui, "Substage", "%u %s", s.substage, SubstageName(s.stage, s.substage));
				RowF(ui, "Next substage", "%u %s", s.nextSubstage, SubstageName(s.stage, s.nextSubstage));
				Row(ui, "In progress", YesNo(s.waveInProgress));
				Row(ui, "Paused", YesNo(s.paused));
				RowF(ui, "Stage progress", "%.4f", s.stageProgress);
				RowF(ui, "Stage duration", "%.2f s", s.stageDuration);
				RowF(ui, "Stage remaining", "%.2f s (%s)", s.stageRemaining, s.stageTimeValid ? "valid" : "UNKNOWN");
				RowF(ui, "Substage remaining", "%.2f s (%s)", s.substageRemaining, s.substageTimeValid ? "valid" : "UNKNOWN");
				RowF(ui, "Next wave in", "%.2f s (%s)", s.nextWaveIn, s.nextWaveTimeValid ? "valid" : "UNKNOWN");
				RowF(ui, "Wave position X", "%.1f (%s)", s.wavePositionX, s.positionValid ? "valid" : "UNKNOWN");
				RowF(ui, "Time since start", "%.1f s", s.timeSinceLastWave);
				ui->EndTable();
			}

			ui->SeparatorText("Sources");
			if (ui->BeginTable("rt_sources", 2, 0))
			{
				Row(ui, "Wave subsystem", s.subsystemResolved ? "resolved" : "NOT RESOLVED");
				Row(ui, "Timer subsystem", s.timerSubsysResolved ? "resolved (offsets verified)" : "not resolved");
				Row(ui, "Timer actor", s.timerActorResolved ? "resolved" : "NOT RESOLVED");
				Row(ui, "Waves stopped", YesNo(s.wavesStopped));
				Row(ui, "NextTime overdue", s.nextWaveOverdue ? "YES - passed with no wave started" : "no");
				Row(ui, "NextTime stale", s.nextWaveTimeStale ? "YES - restored from an earlier session" : "no");
				RowF(ui, "Raw NextTime", "%.2f", s.rawNextTime);
				RowF(ui, "Raw NextPhase", "%d (%s)", s.rawNextPhase,
					s.rawNextPhase == 0 ? "calm/prewave" : (s.rawNextPhase < 0 ? "Cold" : "Heat"));
				RowF(ui, "Timer remaining", "%.2f s", s.rawTimerRemaining);
				if (s.rawCalmTimer < 0.0f)
					Row(ui, "Raw NextWaveTimer", "not read");
				else
					RowF(ui, "Raw NextWaveTimer", "%.2f s", s.rawCalmTimer);
				RowF(ui, "Shown remaining", "%.2f s", s.stageRemaining);
				Row(ui, "Sources disagree", s.sourcesDisagree ? "YES - timer preferred" : "no");
				ui->EndTable();
			}

			if (s.sourcesDisagree)
			{
				ui->Spacing();
				ui->TextWrapped("The replicated timer and the wave subsystem report different "
				                "remaining times. The replicated value is being shown.");
			}

			ui->SeparatorText("Traffic");
			if (ui->BeginTable("rt_traffic", 2, 0))
			{
				RowF(ui, "Sequence", "%u", st.sequence);
				RowF(ui, "Packets received", "%u", st.packetsReceived);
				RowF(ui, "Packets sent", "%u", st.packetsSent);
				RowF(ui, "Broadcast interval", "%.2f s", cfg.broadcastInterval);
				RowF(ui, "Stale after", "%.2f s", cfg.staleAfterSeconds);
				Row(ui, "Local interpolation", YesNo(cfg.countdownInterp));
				ui->EndTable();
			}

#if defined(RUPTURETIMER_DEBUG_TOOLS)
			ui->SeparatorText("Investigation (debug build)");
			ui->TextWrapped("Logs every wave-related object and its reflected properties, with "
			                "offsets and live values. Walks all of GObjects, so it costs a "
			                "~100 MB transient allocation and a visible hitch.");

			const bool pending = ObjectDumper::IsDumpPending();
			ui->BeginDisabled(pending);
			if (ui->Button(pending ? "Dumping..." : "Dump wave objects to log"))
				ObjectDumper::RequestDump();   // runs on the game thread, not here
			ui->EndDisabled();
			ui->SameLine(0.0f, 8.0f);
			ui->TextDisabled("writes to Plugins/logs/modloader.log");

			ui->Spacing();
			ui->TextWrapped("The game's own debug wave RPCs. These are Server RPCs, so they run on "
			                "the server even when clicked from a connected client -- no need to wait "
			                "out the 40 minute schedule.");

			const bool canSend = WaveActions::IsAvailable();
			ui->BeginDisabled(!canSend);
			if (ui->Button("Start heat wave"))   WaveActions::Request(WaveActions::Action::StartHeatWave);
			ui->SameLine(0.0f, 6.0f);
			if (ui->Button("Skip stage"))        WaveActions::Request(WaveActions::Action::SkipToNextStage);
			ui->SameLine(0.0f, 6.0f);
			if (ui->Button("Pause"))             WaveActions::Request(WaveActions::Action::PauseWave);
			ui->SameLine(0.0f, 6.0f);
			if (ui->Button("Cancel wave"))       WaveActions::Request(WaveActions::Action::CancelWave);
			ui->EndDisabled();
			if (!canSend) ui->TextDisabled("no local player controller");
#endif

			ui->Spacing();
			ui->Separator();
			if (ui->Button(Overlay::IsVisible() ? "Hide overlay" : "Show overlay"))
				Overlay::Toggle();
			ui->SameLine(0.0f, 8.0f);
			if (ui->Button("Close"))
				DiagnosticsWindow::Toggle();

			ui->TextDisabled("Reopen from Advanced > ShowDiagnosticsWindow in the config tab.");
		}
	}

	void DiagnosticsWindow::Register()
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->UI || g_widget) return;

		// Title bar, resize, move and scrollbar all stay on — this is a window the
		// user reads and pokes at, not a HUD element.
		g_hints = {};
		g_hints.width     = 440.0f;
		g_hints.height    = 620.0f;
		g_hints.pos_x     = -1.0f;
		g_hints.pos_y     = -1.0f;
		g_hints.size_cond = 1;   // FirstUseEver, so a manual resize sticks
		g_hints.pos_cond  = 1;
		g_hints.extra_window_flags = 0;

		static PluginWidgetDesc desc = { "Rupture Timer - Diagnostics", &Render, &g_hints };
		g_widget = hooks->UI->RegisterWidget(&desc);
		if (!g_widget)
		{
			LOG_ERROR("Failed to register the diagnostics window");
			return;
		}

		hooks->UI->SetWidgetVisible(g_widget, g_visible);
		UpdatePassthrough();
	}

	void DiagnosticsWindow::Unregister()
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->UI) { g_widget = nullptr; return; }

		if (g_passthroughToken)
		{
			hooks->UI->ReleaseInputPassthrough(g_passthroughToken);
			g_passthroughToken = nullptr;
		}
		if (g_widget)
		{
			hooks->UI->UnregisterWidget(g_widget);
			g_widget = nullptr;
		}
	}

	void DiagnosticsWindow::ApplySettings()
	{
		const Settings& cfg = Config::Get();

		if (!cfg.enabled)
		{
			Unregister();
			return;
		}

		if (!g_widget) Register();

		g_visible = cfg.showDiagnostics;

		auto* hooks = GetHooks();
		if (hooks && hooks->UI && g_widget)
			hooks->UI->SetWidgetVisible(g_widget, g_visible);
		UpdatePassthrough();
	}

	void DiagnosticsWindow::Toggle()
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->UI || !g_widget) return;

		g_visible = !g_visible;
		hooks->UI->SetWidgetVisible(g_widget, g_visible);
		UpdatePassthrough();

		// Persist so the config checkbox agrees and ApplySettings won't undo it.
		Config::PersistBool("Advanced", "ShowDiagnosticsWindow", g_visible);
	}

	bool DiagnosticsWindow::IsVisible() { return g_visible; }
}

#endif // MODLOADER_SERVER_BUILD
