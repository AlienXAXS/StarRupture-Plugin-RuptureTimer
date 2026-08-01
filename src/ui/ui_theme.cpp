#include "ui/ui_theme.h"

#include <cmath>

namespace RuptureTimer
{
	namespace
	{
		const Color kAccentPresets[4] = {
			Color(0.24f, 0.72f, 0.98f),  // 0 Signal — cyan
			Color(0.99f, 0.66f, 0.18f),  // 1 Amber
			Color(0.68f, 0.48f, 0.98f),  // 2 Violet
			Color(0.82f, 0.84f, 0.88f),  // 3 Mono
		};

		const Color kHeatAccent = Color(0.99f, 0.42f, 0.16f);
		const Color kColdAccent = Color(0.42f, 0.80f, 0.99f);
	}

	uint32_t Theme::Pack(const Color& c)
	{
		return Pack(c, 1.0f);
	}

	uint32_t Theme::Pack(const Color& c, float alphaMul)
	{
		auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
		auto to8     = [&](float v) { return static_cast<uint32_t>(clamp01(v) * 255.0f + 0.5f); };
		return (to8(c.a * alphaMul) << 24) | (to8(c.b) << 16) | (to8(c.g) << 8) | to8(c.r);
	}

	Color Theme::Mix(const Color& a, const Color& b, float t)
	{
		const float k = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
		return Color(a.r + (b.r - a.r) * k,
		             a.g + (b.g - a.g) * k,
		             a.b + (b.b - a.b) * k,
		             a.a + (b.a - a.a) * k);
	}

	float Theme::Pulse(double time, float periodSeconds)
	{
		if (periodSeconds <= 0.0f) return 0.0f;
		const float phase = static_cast<float>(std::fmod(time, static_cast<double>(periodSeconds))) / periodSeconds;
		return phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f;
	}

	Palette Theme::Build(int preset, bool accentByWaveType, WaveType waveType, float opacity)
	{
		if (preset < 0 || preset > 3) preset = 0;

		Palette p;
		p.accent = kAccentPresets[preset];

		if (accentByWaveType)
		{
			if (waveType == WaveType::Heat)      p.accent = kHeatAccent;
			else if (waveType == WaveType::Cold) p.accent = kColdAccent;
		}

		// Header gradient runs from a deep tint of the accent into the panel body,
		// so the strip reads as "this is what kind of wave is running".
		p.panelTop    = Mix(Color(0.05f, 0.06f, 0.09f), p.accent, 0.55f).WithAlpha(opacity);
		p.panelBottom = Mix(Color(0.05f, 0.06f, 0.09f), p.accent, 0.10f).WithAlpha(opacity);

		p.accentDim   = p.accent.Scaled(0.30f);
		p.background  = Color(0.05f, 0.055f, 0.075f, opacity);
		p.border      = Mix(Color(1.0f, 1.0f, 1.0f), p.accent, 0.6f).WithAlpha(0.22f * opacity + 0.06f);

		p.textPrimary = Color(0.95f, 0.96f, 0.98f);
		p.textMuted   = Color(0.66f, 0.70f, 0.76f);
		p.textFaint   = Color(0.42f, 0.45f, 0.51f);

		p.live        = Color(0.35f, 0.85f, 0.45f);
		p.stale       = Color(0.98f, 0.72f, 0.22f);
		p.waiting     = Color(0.52f, 0.55f, 0.60f);
		p.paused      = Color(0.98f, 0.78f, 0.30f);

		return p;
	}
}
