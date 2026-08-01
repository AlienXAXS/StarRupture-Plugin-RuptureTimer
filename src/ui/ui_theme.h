#pragma once

#include "wave/wave_types.h"

#include <cstdint>

namespace RuptureTimer
{
	struct Color
	{
		float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;

		Color() = default;
		Color(float rr, float gg, float bb, float aa = 1.0f) : r(rr), g(gg), b(bb), a(aa) {}

		Color WithAlpha(float alpha) const { return Color(r, g, b, alpha); }
		Color Scaled(float k) const { return Color(r * k, g * k, b * k, a); }
	};

	// Everything the overlay needs to paint itself for the current wave.
	struct Palette
	{
		Color panelTop;      // header gradient start
		Color panelBottom;   // header gradient end
		Color accent;        // ring fill, active pill, key figures
		Color accentDim;     // inactive track
		Color background;
		Color border;
		Color textPrimary;
		Color textMuted;
		Color textFaint;
		Color live;
		Color stale;
		Color waiting;
		Color paused;
	};

	namespace Theme
	{
		// preset: 0 Signal, 1 Amber, 2 Violet, 3 Mono.
		// accentByWaveType overrides the preset accent with an ember/ice tint.
		Palette Build(int preset, bool accentByWaveType, WaveType waveType, float opacity);

		// Pack for the ImDrawList API (0xAABBGGRR).
		uint32_t Pack(const Color& c);
		uint32_t Pack(const Color& c, float alphaMul);

		Color Mix(const Color& a, const Color& b, float t);

		// 0..1 triangle wave, for the waiting shimmer and the pulse on a live dot.
		float Pulse(double time, float periodSeconds);
	}
}
