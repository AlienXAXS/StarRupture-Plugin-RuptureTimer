#include "ui/overlay_widget.h"

#if defined(MODLOADER_SERVER_BUILD)

// The server has no ImGui surface at all (hooks->UI is null there).
namespace RuptureTimer
{
	void Overlay::Register() {}
	void Overlay::Unregister() {}
	void Overlay::ApplySettings() {}
	void Overlay::Toggle() {}
	bool Overlay::IsVisible() { return false; }
}

#else

#include "ui/ui_draw.h"
#include "ui/ui_theme.h"
#include "net/wave_net.h"
#include "config/plugin_config.h"
#include "plugin_helpers.h"

#include <cstdio>
#include <cstring>

namespace RuptureTimer
{
	namespace
	{
		WidgetHandle       g_widget           = nullptr;
		PluginWindowHints  g_hints            = {};
		bool               g_visible          = true;
		int                g_appliedFlags     = -1;
		void*              g_passthroughToken = nullptr;

		// Base layout, in points at scale 1.0. Heights are derived from these
		// rather than fixed, so turning a row off closes the space instead of
		// leaving a gap above the bottom-anchored timeline.
		constexpr float kBaseWidth       = 320.0f;
		constexpr float kCompactWidth    = 224.0f;
		constexpr float kCompactHeight   = 104.0f;
		constexpr float kPad             = 12.0f;

		constexpr float kHeaderBlock     = 30.0f;   // accent strip + gap under it
		constexpr float kRingBlock       = 64.0f;   // ring diameter; also holds the countdown
		constexpr float kStageNameRow    = 22.0f;
		constexpr float kDetailRow       = 16.0f;
		constexpr float kSmallRow        = 15.0f;
		constexpr float kTinyRow         = 14.0f;
		constexpr float kTimelineGap     = 10.0f;
		constexpr float kTimelinePillH   = 16.0f;

		// Height the full (non-compact) card actually needs for what is enabled.
		float FullCardHeight(const Settings& cfg)
		{
			// Detail column: stage name and type/substage always draw; the rest
			// are optional. Sized on what is *enabled*, not on what happens to be
			// drawable this frame, or the card would resize as the wave changes.
			float textBlock = kStageNameRow + kDetailRow;
			if (cfg.showNextState)     textBlock += kSmallRow;
			if (cfg.showWavePosition)  textBlock += kTinyRow;
			if (cfg.showTimeSinceWave) textBlock += kTinyRow;

			// The ring sets a floor -- it stays 64pt tall however little text
			// sits beside it, and the countdown is centred inside it either way.
			float h = kHeaderBlock + (textBlock > kRingBlock ? textBlock : kRingBlock);

			if (cfg.showStageTimeline) h += kTimelineGap + kTimelinePillH;

			return h + kPad;
		}

		const WaveStage kTimelineStages[5] = {
			WaveStage::PreWave, WaveStage::Moving, WaveStage::Fadeout,
			WaveStage::Growback, WaveStage::None
		};

		int WindowFlagsFor(const Settings& cfg)
		{
			// NoBackground matters: the card below is drawn by hand, so ImGui's own
			// window frame would sit around it as a second, larger box.
			int flags = PluginWindowFlags_NoTitleBar
			          | PluginWindowFlags_NoResize
			          | PluginWindowFlags_NoScrollbar
			          | PluginWindowFlags_NoBackground
			          | PluginWindowFlags_NoSavedSettings;

			// Unlocked leaves ImGui's move handling on, so the card can be dragged
			// by its body whenever a cursor is up (i.e. the ModLoader window is open).
			if (cfg.lockWindow)
				flags |= PluginWindowFlags_NoMove | PluginWindowFlags_NoMouseInputs;

			return flags;
		}

		void UpdateHints(const Settings& cfg)
		{
			const float s = cfg.scale;
			g_hints.width      = (cfg.compactMode ? kCompactWidth : kBaseWidth) * s;
			g_hints.height     = (cfg.compactMode ? kCompactHeight : FullCardHeight(cfg)) * s;
			g_hints.pos_x      = -1.0f;   // let the user place it
			g_hints.pos_y      = -1.0f;
			g_hints.pivot_x    = 0.0f;
			g_hints.pivot_y    = 0.0f;
			g_hints.size_cond  = 0;       // Always, so a scale change lands immediately
			g_hints.pos_cond   = 1;       // FirstUseEver
			g_hints.extra_window_flags = WindowFlagsFor(cfg);
		}

		// While the overlay is unlocked the player is meant to drag it, which needs
		// a cursor. Cooperative passthrough gives ImGui the mouse only when it is
		// actually over the window, so camera look still works.
		void UpdatePassthrough(const Settings& cfg)
		{
			auto* hooks = GetHooks();
			if (!hooks || !hooks->UI) return;

			const bool want = g_visible && cfg.showOverlay && cfg.enabled && !cfg.lockWindow;
			if (want && !g_passthroughToken)
				g_passthroughToken = hooks->UI->AcquireInputPassthrough();
			else if (!want && g_passthroughToken)
			{
				hooks->UI->ReleaseInputPassthrough(g_passthroughToken);
				g_passthroughToken = nullptr;
			}
		}

		struct Layout
		{
			float x0, y0, x1, y1;   // window content rect in screen space
			float scale;
		};

		void DrawHeader(IModLoaderImGui* ui, PluginDrawList dl, const Layout& L,
		                const Palette& pal, const DisplayState& st)
		{
			const float s = L.scale;

			// Background card + accent gradient strip along the top edge.
			ui->DL_AddRectFilled(dl, L.x0, L.y0, L.x1, L.y1, Theme::Pack(pal.background), 8.0f * s,
				PluginDrawFlags_RoundCornersAll);
			ui->DL_AddRectFilledMultiColor(dl, L.x0, L.y0, L.x1, L.y0 + 26.0f * s,
				Theme::Pack(pal.panelTop), Theme::Pack(pal.panelTop),
				Theme::Pack(pal.panelBottom), Theme::Pack(pal.panelBottom));
			ui->DL_AddRect(dl, L.x0, L.y0, L.x1, L.y1, Theme::Pack(pal.border), 8.0f * s,
				PluginDrawFlags_RoundCornersAll, 1.0f);

			Draw::TextAt(ui, dl, L.x0 + kPad * s, L.y0 + 6.0f * s, "RUPTURE WAVE",
				pal.textPrimary, 13.0f * s);

			const Settings& cfg = Config::Get();
			if (!cfg.showNetworkStatus) return;

			// Role badge and freshness dot, right-aligned.
			float cursorX = L.x1 - kPad * s;

			const Color dotColor = !st.hasData ? pal.waiting : (st.stale ? pal.stale : pal.live);
			const float glow     = st.hasData && !st.stale ? Theme::Pulse(ui->GetTime(), 2.4f) : 0.0f;
			Draw::StatusDot(ui, dl, cursorX - 4.0f * s, L.y0 + 13.0f * s, 3.2f * s, dotColor, glow);
			cursorX -= 14.0f * s;

			const float badgeFont = 11.0f * s;
			const float badgeY    = L.y0 + 5.0f * s;

			const char* role      = RoleName(st.role);
			const float roleWidth = Draw::BadgeWidth(ui, role, badgeFont);
			Draw::Badge(ui, dl, cursorX - roleWidth, badgeY, role,
				pal.accent.WithAlpha(0.22f), pal.textPrimary, badgeFont);
			cursorX -= roleWidth + 6.0f * s;

			if (st.hasData && st.snapshot.paused)
			{
				const float pausedWidth = Draw::BadgeWidth(ui, "PAUSED", badgeFont);
				Draw::Badge(ui, dl, cursorX - pausedWidth, badgeY, "PAUSED",
					pal.paused.WithAlpha(0.28f), pal.paused, badgeFont);
			}
		}

		void DrawWaitingBody(IModLoaderImGui* ui, PluginDrawList dl, const Layout& L,
		                     const Palette& pal, const DisplayState& st)
		{
			const float s  = L.scale;
			const float cx = (L.x0 + L.x1) * 0.5f;
			const float cy = (L.y0 + 26.0f * s + L.y1) * 0.5f;

			// Faint ring with no fill: the shape of the real readout, deliberately empty.
			Draw::Ring(ui, dl, cx, cy - 10.0f * s, 26.0f * s, 4.0f * s, 0.0f,
				pal.accentDim.WithAlpha(0.35f), pal.accentDim);
			Draw::TextCentered(ui, dl, cx, cy - 22.0f * s, "--:--", pal.textFaint, 22.0f * s);

			// Indeterminate shimmer — motion without implying progress.
			const float barW = (L.x1 - L.x0) - kPad * 2.0f * s;
			const float barY = cy + 24.0f * s;
			const Rect  bar  = { L.x0 + kPad * s, barY, L.x1 - kPad * s, barY + 3.0f * s };
			ui->DL_AddRectFilled(dl, bar.x0, bar.y0, bar.x1, bar.y1,
				Theme::Pack(pal.accentDim.WithAlpha(0.30f)), 1.5f * s, PluginDrawFlags_RoundCornersAll);

			const float t     = Theme::Pulse(ui->GetTime(), 1.8f);
			const float chunk = barW * 0.28f;
			const float sx    = bar.x0 + (barW - chunk) * t;
			ui->DL_AddRectFilled(dl, sx, bar.y0, sx + chunk, bar.y1,
				Theme::Pack(pal.accent.WithAlpha(0.75f)), 1.5f * s, PluginDrawFlags_RoundCornersAll);

			Draw::TextCentered(ui, dl, cx, barY + 10.0f * s, WaitReasonText(st.waitReason),
				pal.textMuted, 12.0f * s);
		}

		void DrawTimeline(IModLoaderImGui* ui, PluginDrawList dl, const Layout& L,
		                  const Palette& pal, const WaveSnapshot& snap)
		{
			const float s      = L.scale;
			const float gap    = 4.0f * s;
			const float pillH  = kTimelinePillH * s;
			const float totalW = (L.x1 - L.x0) - kPad * 2.0f * s;
			const float pillW  = (totalW - gap * 4.0f) / 5.0f;
			const float y      = L.y1 - kPad * s - pillH;

			for (int i = 0; i < 5; ++i)
			{
				const WaveStage stage  = kTimelineStages[i];
				const bool      active = (stage == snap.stage);
				const float     x      = L.x0 + kPad * s + (pillW + gap) * i;
				const Rect      r      = { x, y, x + pillW, y + pillH };

				const Color bg = active ? pal.accent.WithAlpha(0.20f) : Color(1.0f, 1.0f, 1.0f, 0.05f);
				const Color fg = active ? pal.textPrimary : pal.textFaint;

				Draw::Pill(ui, dl, r, StageShortName(stage), bg, fg,
					active ? snap.stageProgress : 0.0f, pal.accent.WithAlpha(0.42f));
			}
		}

		void DrawDataBody(IModLoaderImGui* ui, PluginDrawList dl, const Layout& L,
		                  const Palette& pal, const DisplayState& st)
		{
			const Settings& cfg  = Config::Get();
			const WaveSnapshot& snap = st.snapshot;
			const float s = L.scale;

			// These row heights must match FullCardHeight above, or the card is
			// sized for a layout it does not draw.
			const float bodyTop = L.y0 + kHeaderBlock * s;

			// --- Countdown ring -------------------------------------------------
			const float ringR = kRingBlock * 0.5f * s - 2.0f * s;
			const float ringX = L.x0 + kPad * s + ringR;
			const float ringY = bodyTop + ringR + 2.0f * s;

			char timeBuf[24];
			Draw::FormatDuration(timeBuf, sizeof(timeBuf), snap.stageRemaining, snap.stageTimeValid);

			if (cfg.showProgressRing)
			{
				Draw::Ring(ui, dl, ringX, ringY, ringR, 4.5f * s,
					snap.stageTimeValid ? snap.stageProgress : 0.0f,
					pal.accentDim.WithAlpha(0.45f), pal.accent);
			}
			Draw::TextCentered(ui, dl, ringX, ringY - 11.0f * s, timeBuf, pal.textPrimary, 22.0f * s);

			// --- Detail column ---------------------------------------------------
			const float colX = ringX + ringR + 14.0f * s;
			float       ty   = bodyTop + 2.0f * s;

			Draw::TextAt(ui, dl, colX, ty, StageName(snap.stage), pal.accent, 19.0f * s);
			ty += kStageNameRow * s;

			// U+00B7 is inside the base font's preloaded Latin-1 range, so unlike
			// the arrow below it needs no icon font.
			char detail[128];
			const char* sub = SubstageName(snap.stage, snap.substage);
			if (snap.wavesStopped)
				snprintf(detail, sizeof(detail), "Waves stopped");
			else if (snap.nextWaveTimeStale)
				snprintf(detail, sizeof(detail), "Timer from an earlier session");
			else if (snap.nextWaveOverdue)
				snprintf(detail, sizeof(detail), "Overdue \xC2\xB7 no wave started yet");
			else if (cfg.showSubstage && sub && sub[0])
				snprintf(detail, sizeof(detail), "%s  \xC2\xB7  %s", WaveTypeName(snap.waveType), sub);
			else if (snap.waveInProgress)
				snprintf(detail, sizeof(detail), "%s", WaveTypeName(snap.waveType));
			else
				snprintf(detail, sizeof(detail), "Counting down to the next Rupture");
			Draw::TextAt(ui, dl, colX, ty, detail, pal.textMuted, 12.0f * s);
			ty += kDetailRow * s;

			if (cfg.showNextState)
			{
				char next[128];
				if (snap.wavesStopped)
				{
					// Nothing is scheduled, so promising a next stage would be a lie.
					snprintf(next, sizeof(next), "NEXT %s none scheduled", Icon::PlayArrow);
				}
				else
				{
					const char* nextSub = SubstageName(snap.stage, snap.nextSubstage);
					const char* target  = (cfg.showSubstage && nextSub && nextSub[0])
						? nextSub                   // next change is within this stage
						: StageName(snap.nextStage);
					snprintf(next, sizeof(next), "NEXT %s %s", Icon::PlayArrow, target);
				}
				Draw::TextAt(ui, dl, colX, ty, next, pal.textFaint, 12.0f * s);
				ty += kSmallRow * s;
			}

			// These two are sized into the card whenever they are enabled, so the
			// row advances even on a frame where there is nothing to print --
			// otherwise the layout would shift as the wave moves in and out of
			// the stages that populate them.
			if (cfg.showWavePosition)
			{
				if (snap.positionValid)
				{
					char pos[64];
					snprintf(pos, sizeof(pos), "Position  %.0f", snap.wavePositionX);
					Draw::TextAt(ui, dl, colX, ty, pos, pal.textFaint, 11.0f * s);
				}
				ty += kTinyRow * s;
			}

			if (cfg.showTimeSinceWave && snap.waveInProgress)
			{
				char since[64];
				char sinceTime[24];
				Draw::FormatDuration(sinceTime, sizeof(sinceTime), snap.timeSinceLastWave, true);
				snprintf(since, sizeof(since), "Elapsed  %s", sinceTime);
				Draw::TextAt(ui, dl, colX, ty, since, pal.textFaint, 11.0f * s);
			}
			(void)ty;

			if (cfg.showStageTimeline && !cfg.compactMode)
				DrawTimeline(ui, dl, L, pal, snap);
		}

		void DrawCompactBody(IModLoaderImGui* ui, PluginDrawList dl, const Layout& L,
		                     const Palette& pal, const DisplayState& st)
		{
			const float s  = L.scale;
			const float cx = (L.x0 + L.x1) * 0.5f;

			char timeBuf[24];
			Draw::FormatDuration(timeBuf, sizeof(timeBuf), st.snapshot.stageRemaining,
				st.snapshot.stageTimeValid);

			Draw::TextCentered(ui, dl, cx, L.y0 + 32.0f * s, timeBuf, pal.textPrimary, 30.0f * s);
			Draw::TextCentered(ui, dl, cx, L.y0 + 66.0f * s, StageName(st.snapshot.stage),
				pal.accent, 14.0f * s);

			const Rect bar = { L.x0 + kPad * s, L.y1 - kPad * s - 4.0f * s,
			                   L.x1 - kPad * s, L.y1 - kPad * s };
			Draw::TrackBar(ui, dl, bar, st.snapshot.stageTimeValid ? st.snapshot.stageProgress : 0.0f,
				pal.accentDim.WithAlpha(0.35f), pal.accent, 2.0f * s);
		}

		void Render(IModLoaderImGui* ui)
		{
			const Settings& cfg = Config::Get();
			if (!cfg.enabled || !cfg.showOverlay) return;

			const DisplayState st = WaveNet::GetDisplayState();

			// Waiting states must not be tinted by a wave type we are not sure about.
			const WaveType accentType = st.hasData ? st.snapshot.waveType : WaveType::None;
			const Palette  pal = Theme::Build(cfg.accentPreset, cfg.accentByWaveType,
				accentType, cfg.opacity);

			// Draw over the whole window rect rather than the padded content
			// region -- with NoBackground the padding is just dead transparent
			// margin, and the card should fill what the size hint asked for.
			float wx = 0.0f, wy = 0.0f, ww = 0.0f, wh = 0.0f;
			ui->GetWindowPos(&wx, &wy);
			ui->GetWindowSize(&ww, &wh);
			if (ww <= 1.0f || wh <= 1.0f) return;

			// Still reserve the content region so ImGui keeps the window sized and
			// leaves the body draggable.
			float aw = 0.0f, ah = 0.0f;
			ui->GetContentRegionAvail(&aw, &ah);
			if (aw > 0.0f && ah > 0.0f) ui->Dummy(aw, ah);

			Layout L{ wx, wy, wx + ww, wy + wh, cfg.scale };
			PluginDrawList dl = ui->GetWindowDrawList();

			DrawHeader(ui, dl, L, pal, st);

			if (!st.hasData)
				DrawWaitingBody(ui, dl, L, pal, st);
			else if (cfg.compactMode)
				DrawCompactBody(ui, dl, L, pal, st);
			else
				DrawDataBody(ui, dl, L, pal, st);
		}
	}

	void Overlay::Register()
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->UI || g_widget) return;

		const Settings& cfg = Config::Get();
		UpdateHints(cfg);
		g_appliedFlags = g_hints.extra_window_flags;

		static PluginWidgetDesc desc = { "RuptureTimer_Overlay", &Render, &g_hints };
		g_widget = hooks->UI->RegisterWidget(&desc);
		if (!g_widget)
		{
			LOG_ERROR("Failed to register the overlay widget");
			return;
		}

		g_visible = cfg.showOverlay;
		hooks->UI->SetWidgetVisible(g_widget, g_visible);
		UpdatePassthrough(cfg);
	}

	void Overlay::Unregister()
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
		g_appliedFlags = -1;
	}

	void Overlay::ApplySettings()
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->UI) return;

		const Settings& cfg = Config::Get();

		if (!cfg.enabled)
		{
			Unregister();
			return;
		}

		UpdateHints(cfg);

		// Window flags are baked in when the widget is created, so a lock/unlock
		// needs the widget rebuilt. Size and colour changes land on their own.
		if (g_widget && g_hints.extra_window_flags != g_appliedFlags)
		{
			Unregister();
			Register();
			return;
		}

		if (!g_widget)
		{
			Register();
			return;
		}

		g_visible = cfg.showOverlay;
		if (g_widget) hooks->UI->SetWidgetVisible(g_widget, g_visible);
		UpdatePassthrough(cfg);
	}

	void Overlay::Toggle()
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->UI || !g_widget) return;

		g_visible = !g_visible;
		hooks->UI->SetWidgetVisible(g_widget, g_visible);
		UpdatePassthrough(Config::Get());

		// Persist so the config checkbox agrees and ApplySettings won't undo it.
		Config::PersistBool("General", "ShowOverlay", g_visible);
	}

	bool Overlay::IsVisible() { return g_visible; }
}

#endif // MODLOADER_SERVER_BUILD
