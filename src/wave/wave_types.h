#pragma once

#include <cstdint>

// Build-independent mirror of the game's wave state. Everything downstream of the
// authority reader (networking, UI) works with this, never with SDK types — the
// UI runs on the render thread and must not touch UObjects.
namespace RuptureTimer
{
	// Mirrors SDK::EEnviroWaveStage.
	enum class WaveStage : uint8_t
	{
		None     = 0,   // calm — no wave running
		PreWave  = 1,
		Moving   = 2,
		Fadeout  = 3,
		Growback = 4,
	};

	// Mirrors SDK::EEnviroWave.
	enum class WaveType : uint8_t
	{
		None = 0,
		Heat = 1,
		Cold = 2,
	};

	// The three stages that have substages each use their own enum in the SDK.
	// They are stored here as a raw value plus the stage they belong to.
	enum class PreWaveSubstage  : uint8_t { None = 0, BeforeExplosion = 1, AfterExplosion = 2 };
	enum class FadeoutSubstage  : uint8_t { None = 0, FireWave = 1, Burning = 2, Fading = 3 };
	enum class GrowbackSubstage : uint8_t { None = 0, MoonPhase = 1, RegrowthStart = 2, Regrowth = 3 };

	// Where the plugin sits in the session. Resolved from EPluginNetMode, which
	// returns Unknown until an actor exists — hence the explicit Unresolved value.
	enum class SessionRole : uint8_t
	{
		Unresolved      = 0,
		Solo            = 1,   // Standalone
		ListenHost      = 2,   // ListenServer  (authority + UI)
		DedicatedServer = 3,   // authority, no UI
		RemoteClient    = 4,   // consumer
	};

	inline bool RoleHasAuthority(SessionRole r)
	{
		return r == SessionRole::Solo || r == SessionRole::ListenHost || r == SessionRole::DedicatedServer;
	}

	// Why the display has nothing to show. Surfaced verbatim in the overlay so the
	// user can tell "not connected yet" from "server has no plugin".
	enum class WaitReason : uint8_t
	{
		None = 0,
		DetectingSession,     // net mode still Unknown
		NoWorld,              // world/game state not up yet
		SubsystemUnresolved,  // authority: UCrEnviroWaveSubsystem not found
		NoServerData,         // client: never received a packet
		LinkStale,            // client: last packet older than StaleAfterSeconds
	};

	// Per-field validity. The wave math can legitimately fail (a zero WaveSpeed, a
	// stage the replicated timer cannot express), and in those cases the UI prints
	// an em dash rather than a made-up number.
	struct WaveSnapshot
	{
		WaveStage stage        = WaveStage::None;
		WaveStage nextStage    = WaveStage::None;
		WaveType  waveType     = WaveType::None;
		uint8_t   substage     = 0;   // value of the enum matching `stage`
		uint8_t   nextSubstage = 0;

		bool waveInProgress = false;
		bool paused         = false;
		bool wavesStopped   = false;   // waves disabled outright — no next one coming

		// NextTime is armed once at world start as WaitingDuration + ~0, so it is
		// a genuine countdown to the session's first wave. If that moment passes
		// without a wave starting, nothing rearms it and it just slides further
		// into the past -- the schedule has stalled, and there is no next-wave
		// time to show until a wave event fires.
		bool nextWaveOverdue = false;

		// The opposite failure: NextTime is further away than a calm window can
		// possibly be, which means it is an absolute timestamp restored from the
		// session that wrote the save, measured against a clock that no longer
		// exists. Distinct from overdue, and worth saying so -- the causes and
		// the fixes are different.
		bool nextWaveTimeStale = false;

		float stageProgress     = 0.0f;   // 0..1 within the current stage
		float stageDuration     = 0.0f;   // seconds
		float stageRemaining    = 0.0f;   // seconds until nextStage
		float substageRemaining = 0.0f;
		float nextWaveIn        = 0.0f;   // calm-window countdown
		float wavePositionX     = 0.0f;
		float timeSinceLastWave = 0.0f;

		bool stageTimeValid     = false;
		bool nextWaveTimeValid  = false;
		bool substageTimeValid  = false;
		bool positionValid      = false;

		// Diagnostics — carried over the wire so a client can see why the server
		// is unsure, not just that it is.
		bool subsystemResolved  = false;
		bool timerActorResolved = false;
		bool timerSubsysResolved = false;
		bool sourcesDisagree    = false;

		// Raw source values, for the diagnostics window. Not used for display.
		float   rawNextTime       = 0.0f;
		int32_t rawNextPhase      = 0;
		float   rawTimerRemaining = 0.0f;
		float   rawCalmTimer      = -1.0f;  // -1 = timer subsystem never read
	};

	const char* StageName(WaveStage s);
	const char* StageShortName(WaveStage s);
	const char* WaveTypeName(WaveType t);
	const char* SubstageName(WaveStage stage, uint8_t substage);
	const char* RoleName(SessionRole r);
	const char* WaitReasonText(WaitReason r);
}
