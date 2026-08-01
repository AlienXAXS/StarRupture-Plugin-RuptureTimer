#pragma once

#include "plugin_interface.h"
#include "ui/ui_theme.h"

namespace RuptureTimer
{
	struct Rect { float x0, y0, x1, y1; };

	// Draw-list primitives shared by the overlay and the panel. Every one of these
	// must be called from inside a render callback — the PluginDrawList handle is
	// only valid for the current frame.
	namespace Draw
	{
		// Rounded bar with a filled portion, used for progress and the shimmer.
		void TrackBar(IModLoaderImGui* ui, PluginDrawList dl, const Rect& r,
		              float fraction, const Color& track, const Color& fill, float rounding);

		// Rounded pill with a label centred inside it, optionally filled to
		// `fraction` to show progress sweeping through the active stage.
		void Pill(IModLoaderImGui* ui, PluginDrawList dl, const Rect& r, const char* label,
		          const Color& bg, const Color& fg, float fraction, const Color& fillColor);

		// Circular gauge: a full faint ring plus an arc covering `fraction`,
		// starting at 12 o'clock and sweeping clockwise.
		void Ring(IModLoaderImGui* ui, PluginDrawList dl, float cx, float cy, float radius,
		          float thickness, float fraction, const Color& track, const Color& fill);

		// Text helpers that centre on a point rather than the layout cursor.
		void TextCentered(IModLoaderImGui* ui, PluginDrawList dl, float cx, float y,
		                  const char* text, const Color& c, float fontSize);
		void TextAt(IModLoaderImGui* ui, PluginDrawList dl, float x, float y,
		            const char* text, const Color& c, float fontSize);

		// Small status dot with a soft halo.
		void StatusDot(IModLoaderImGui* ui, PluginDrawList dl, float cx, float cy,
		               float radius, const Color& c, float glow);

		// Uppercase chip used for the role and PAUSED badges. Returns its width so
		// several can be laid out right-to-left.
		float Badge(IModLoaderImGui* ui, PluginDrawList dl, float x, float y,
		            const char* text, const Color& bg, const Color& fg, float fontSize);

		// Width Badge() would occupy, without drawing anything.
		float BadgeWidth(IModLoaderImGui* ui, const char* text, float fontSize);

		// "M:SS" for anything under an hour, "H:MM:SS" beyond. Writes "--:--"
		// when the value is not trusted, so the UI never invents a number.
		void FormatDuration(char* buf, size_t bufSize, float seconds, bool valid);
	}

	// Glyphs from the Material Icons font the ModLoader merges into the atlas.
	// The base text font only preloads U+0020-U+00FF plus Cyrillic, so anything
	// fancier than Latin-1 has to come from here or it renders as '?'.
	// See MaterialIcons.md in the ModLoader repo.
	namespace Icon
	{
		constexpr const char* PlayArrow = "\xEE\x80\xB7";   // U+E037
	}
}
