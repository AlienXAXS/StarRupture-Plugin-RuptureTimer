#include "ui/ui_draw.h"

#include <cmath>
#include <cstdio>

namespace RuptureTimer
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;

		float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
	}

	void Draw::TrackBar(IModLoaderImGui* ui, PluginDrawList dl, const Rect& r,
	                    float fraction, const Color& track, const Color& fill, float rounding)
	{
		ui->DL_AddRectFilled(dl, r.x0, r.y0, r.x1, r.y1, Theme::Pack(track), rounding, PluginDrawFlags_RoundCornersAll);

		const float f = Clamp01(fraction);
		if (f <= 0.0f) return;

		const float w = (r.x1 - r.x0) * f;
		// A sliver narrower than the rounding radius renders as a blob, so clamp.
		const float end = r.x0 + (w < rounding * 2.0f ? rounding * 2.0f : w);
		ui->DL_AddRectFilled(dl, r.x0, r.y0, end > r.x1 ? r.x1 : end, r.y1,
			Theme::Pack(fill), rounding, PluginDrawFlags_RoundCornersAll);
	}

	void Draw::Pill(IModLoaderImGui* ui, PluginDrawList dl, const Rect& r, const char* label,
	                const Color& bg, const Color& fg, float fraction, const Color& fillColor)
	{
		const float h        = r.y1 - r.y0;
		const float rounding = h * 0.5f;

		ui->DL_AddRectFilled(dl, r.x0, r.y0, r.x1, r.y1, Theme::Pack(bg), rounding, PluginDrawFlags_RoundCornersAll);

		const float f = Clamp01(fraction);
		if (f > 0.0f)
		{
			// Clip the sweep so the fill keeps the pill's rounded silhouette.
			ui->DL_PushClipRect(dl, r.x0, r.y0, r.x0 + (r.x1 - r.x0) * f, r.y1, true);
			ui->DL_AddRectFilled(dl, r.x0, r.y0, r.x1, r.y1, Theme::Pack(fillColor), rounding, PluginDrawFlags_RoundCornersAll);
			ui->DL_PopClipRect(dl);
		}

		if (label && label[0])
		{
			float tw = 0.0f, th = 0.0f;
			ui->CalcTextSize(label, &tw, &th, false, 0.0f);
			ui->DL_AddText(dl, r.x0 + ((r.x1 - r.x0) - tw) * 0.5f, r.y0 + (h - th) * 0.5f,
				Theme::Pack(fg), label);
		}
	}

	void Draw::Ring(IModLoaderImGui* ui, PluginDrawList dl, float cx, float cy, float radius,
	                float thickness, float fraction, const Color& track, const Color& fill)
	{
		// ImGui angles run from +X counter-clockwise, so 12 o'clock is -pi/2.
		constexpr float kStart = -kPi * 0.5f;

		ui->DL_PathClear(dl);
		ui->DL_PathArcTo(dl, cx, cy, radius, kStart, kStart + kPi * 2.0f, 96);
		ui->DL_PathStroke(dl, Theme::Pack(track), PluginDrawFlags_None, thickness);

		const float f = Clamp01(fraction);
		if (f <= 0.0f) return;

		ui->DL_PathClear(dl);
		ui->DL_PathArcTo(dl, cx, cy, radius, kStart, kStart + kPi * 2.0f * f,
			static_cast<int>(16.0f + 80.0f * f));
		ui->DL_PathStroke(dl, Theme::Pack(fill), PluginDrawFlags_None, thickness);

		// A dot on the leading edge makes the sweep direction obvious at a glance.
		const float a = kStart + kPi * 2.0f * f;
		ui->DL_AddCircleFilled(dl, cx + std::cos(a) * radius, cy + std::sin(a) * radius,
			thickness * 0.62f, Theme::Pack(fill), 12);
	}

	void Draw::TextCentered(IModLoaderImGui* ui, PluginDrawList dl, float cx, float y,
	                        const char* text, const Color& c, float fontSize)
	{
		if (!text || !text[0]) return;
		float tw = 0.0f, th = 0.0f;
		ui->CalcTextSize(text, &tw, &th, false, 0.0f);

		const float base = ui->GetFontSize();
		const float k    = (fontSize > 0.0f && base > 0.0f) ? fontSize / base : 1.0f;
		ui->DL_AddTextSized(dl, fontSize > 0.0f ? fontSize : base, cx - (tw * k) * 0.5f, y,
			Theme::Pack(c), text);
	}

	void Draw::TextAt(IModLoaderImGui* ui, PluginDrawList dl, float x, float y,
	                  const char* text, const Color& c, float fontSize)
	{
		if (!text || !text[0]) return;
		const float base = ui->GetFontSize();
		ui->DL_AddTextSized(dl, fontSize > 0.0f ? fontSize : base, x, y, Theme::Pack(c), text);
	}

	void Draw::StatusDot(IModLoaderImGui* ui, PluginDrawList dl, float cx, float cy,
	                     float radius, const Color& c, float glow)
	{
		if (glow > 0.0f)
			ui->DL_AddCircleFilled(dl, cx, cy, radius * (1.9f + glow * 0.9f),
				Theme::Pack(c, 0.16f * glow + 0.05f), 16);
		ui->DL_AddCircleFilled(dl, cx, cy, radius, Theme::Pack(c), 16);
	}

	namespace
	{
		// Shared geometry so BadgeWidth and Badge can never drift apart.
		void BadgeMetrics(IModLoaderImGui* ui, const char* text, float fontSize,
		                  float& w, float& h, float& padX, float& padY)
		{
			float tw = 0.0f, th = 0.0f;
			ui->CalcTextSize(text, &tw, &th, false, 0.0f);

			const float base = ui->GetFontSize();
			const float k    = (fontSize > 0.0f && base > 0.0f) ? fontSize / base : 1.0f;

			w    = tw * k;
			h    = th * k;
			padX = h * 0.45f;
			padY = h * 0.16f;
		}
	}

	float Draw::BadgeWidth(IModLoaderImGui* ui, const char* text, float fontSize)
	{
		if (!text || !text[0]) return 0.0f;
		float w, h, padX, padY;
		BadgeMetrics(ui, text, fontSize, w, h, padX, padY);
		return w + padX * 2.0f;
	}

	float Draw::Badge(IModLoaderImGui* ui, PluginDrawList dl, float x, float y,
	                  const char* text, const Color& bg, const Color& fg, float fontSize)
	{
		if (!text || !text[0]) return 0.0f;

		float w, h, padX, padY;
		BadgeMetrics(ui, text, fontSize, w, h, padX, padY);

		const float base = ui->GetFontSize();
		const float x1   = x + w + padX * 2.0f;
		const float y1   = y + h + padY * 2.0f;

		ui->DL_AddRectFilled(dl, x, y, x1, y1, Theme::Pack(bg), (y1 - y) * 0.35f, PluginDrawFlags_RoundCornersAll);
		ui->DL_AddTextSized(dl, fontSize > 0.0f ? fontSize : base, x + padX, y + padY, Theme::Pack(fg), text);

		return x1 - x;
	}

	void Draw::FormatDuration(char* buf, size_t bufSize, float seconds, bool valid)
	{
		if (!buf || bufSize == 0) return;

		if (!valid || !std::isfinite(seconds) || seconds < 0.0f)
		{
			// Unknown, not zero -- and shaped like a clock so it reads as a
			// missing reading rather than a stopped one.
			snprintf(buf, bufSize, "--:--");
			return;
		}

		const int total = static_cast<int>(seconds + 0.5f);
		const int h     = total / 3600;
		const int m     = (total % 3600) / 60;
		const int s     = total % 60;

		if (h > 0) snprintf(buf, bufSize, "%d:%02d:%02d", h, m, s);
		else       snprintf(buf, bufSize, "%d:%02d", m, s);
	}
}
