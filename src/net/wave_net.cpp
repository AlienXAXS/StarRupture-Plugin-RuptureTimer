#include "net/wave_net.h"
#include "net/wave_packet.h"
#include "wave/wave_reader.h"
#include "wave/wave_model.h"
#include "config/plugin_config.h"
#include "plugin_helpers.h"

#include "plugin_network_helpers.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <vector>

namespace RuptureTimer
{
	namespace
	{
		std::mutex   g_mutex;
		DisplayState g_display;

		SessionRole g_role = SessionRole::Unresolved;

		// Authority side
		float        g_broadcastAccum = 0.0f;
		float        g_pollAccum      = 0.0f;
		uint32_t     g_sequence       = 0;
		WaveSnapshot g_lastSentSnapshot;
		bool         g_hasSentAnything = false;

		// Authority side: the clients the loader will actually deliver to. A client
		// only lands here once it has reported this plugin at this exact version,
		// which is strictly later than the player-joined hook.
		std::vector<void*> g_readyClients;

		// Client side
		bool     g_serverReady         = false;
		float    g_helloRetryCooldown  = 0.0f;

		// Which readiness callbacks we currently hold, so the per-session teardown
		// only unregisters what it registered.
		bool g_clientReadyRegistered = false;
		bool g_serverReadyRegistered = false;

		PluginNetworkMessageCallback       g_waveHandler  = nullptr;
		PluginNetworkServerMessageCallback g_helloHandler = nullptr;

		constexpr float kPollIntervalSeconds  = 0.25f;
		constexpr float kHelloRetrySeconds    = 10.0f;

		double NowSeconds()
		{
			using namespace std::chrono;
			return duration<double>(steady_clock::now().time_since_epoch()).count();
		}

		double g_lastPacketTime = 0.0;

		SessionRole RoleFromNetMode(EPluginNetMode mode)
		{
			switch (mode)
			{
			case EPluginNetMode::Standalone:      return SessionRole::Solo;
			case EPluginNetMode::ListenServer:    return SessionRole::ListenHost;
			case EPluginNetMode::DedicatedServer: return SessionRole::DedicatedServer;
			case EPluginNetMode::Client:          return SessionRole::RemoteClient;
			default:                              return SessionRole::Unresolved;
			}
		}

		RuptureWavePacket Encode(const WaveSnapshot& s, uint32_t sequence)
		{
			RuptureWavePacket p{};
			p.schemaVersion     = kSchemaVersion;
			p.waveType          = static_cast<uint8_t>(s.waveType);
			p.stage             = static_cast<uint8_t>(s.stage);
			p.nextStage         = static_cast<uint8_t>(s.nextStage);
			p.substage          = s.substage;
			p.nextSubstage      = s.nextSubstage;
			p.sequence          = sequence;
			p.stageProgress     = s.stageProgress;
			p.stageDuration     = s.stageDuration;
			p.stageRemaining    = s.stageRemaining;
			p.substageRemaining = s.substageRemaining;
			p.nextWaveIn        = s.nextWaveIn;
			p.wavePositionX     = s.wavePositionX;
			p.timeSinceLastWave = s.timeSinceLastWave;
			p.rawNextTime       = s.rawNextTime;
			p.rawNextPhase      = s.rawNextPhase;
			p.rawTimerRemaining = s.rawTimerRemaining;
			p.rawCalmTimer      = s.rawCalmTimer;

			uint16_t f = PF_None;
			if (s.waveInProgress)     f |= PF_WaveInProgress;
			if (s.paused)             f |= PF_Paused;
			if (s.stageTimeValid)     f |= PF_StageTimeValid;
			if (s.nextWaveTimeValid)  f |= PF_NextWaveTimeValid;
			if (s.substageTimeValid)  f |= PF_SubstageTimeValid;
			if (s.positionValid)      f |= PF_PositionValid;
			if (s.subsystemResolved)   f |= PF_SubsystemResolved;
			if (s.timerActorResolved)  f |= PF_TimerActorResolved;
			if (s.sourcesDisagree)     f |= PF_SourcesDisagree;
			if (s.wavesStopped)        f |= PF_WavesStopped;
			if (s.timerSubsysResolved) f |= PF_TimerSubsysResolved;
			if (s.nextWaveOverdue)   f |= PF_NextWaveOverdue;
			if (s.nextWaveTimeStale) f |= PF_NextWaveStale;
			p.flags = f;

			return p;
		}

		WaveSnapshot Decode(const RuptureWavePacket& p)
		{
			WaveSnapshot s;
			s.waveType          = static_cast<WaveType>(p.waveType);
			s.stage             = static_cast<WaveStage>(p.stage);
			s.nextStage         = static_cast<WaveStage>(p.nextStage);
			s.substage          = p.substage;
			s.nextSubstage      = p.nextSubstage;
			s.stageProgress     = p.stageProgress;
			s.stageDuration     = p.stageDuration;
			s.stageRemaining    = p.stageRemaining;
			s.substageRemaining = p.substageRemaining;
			s.nextWaveIn        = p.nextWaveIn;
			s.wavePositionX     = p.wavePositionX;
			s.timeSinceLastWave = p.timeSinceLastWave;
			s.rawNextTime       = p.rawNextTime;
			s.rawNextPhase      = p.rawNextPhase;
			s.rawTimerRemaining = p.rawTimerRemaining;
			s.rawCalmTimer      = p.rawCalmTimer;

			s.waveInProgress     = (p.flags & PF_WaveInProgress)     != 0;
			s.paused             = (p.flags & PF_Paused)             != 0;
			s.stageTimeValid     = (p.flags & PF_StageTimeValid)     != 0;
			s.nextWaveTimeValid  = (p.flags & PF_NextWaveTimeValid)  != 0;
			s.substageTimeValid  = (p.flags & PF_SubstageTimeValid)  != 0;
			s.positionValid      = (p.flags & PF_PositionValid)      != 0;
			s.subsystemResolved   = (p.flags & PF_SubsystemResolved)   != 0;
			s.timerActorResolved  = (p.flags & PF_TimerActorResolved)  != 0;
			s.sourcesDisagree     = (p.flags & PF_SourcesDisagree)     != 0;
			s.wavesStopped        = (p.flags & PF_WavesStopped)        != 0;
			s.timerSubsysResolved = (p.flags & PF_TimerSubsysResolved) != 0;
			s.nextWaveOverdue   = (p.flags & PF_NextWaveOverdue)   != 0;
			s.nextWaveTimeStale = (p.flags & PF_NextWaveStale)     != 0;
			return s;
		}

		// A transition the player would notice, worth an out-of-band packet
		// instead of waiting out the rest of the broadcast interval.
		bool IsNotableTransition(const WaveSnapshot& a, const WaveSnapshot& b)
		{
			return a.stage != b.stage
			    || a.waveType != b.waveType
			    || a.substage != b.substage
			    || a.paused != b.paused
			    || a.waveInProgress != b.waveInProgress
			    || a.wavesStopped != b.wavesStopped
			    || a.stageTimeValid != b.stageTimeValid;
		}

		void Broadcast(const WaveSnapshot& s)
		{
			auto* self  = GetSelf();
			auto* hooks = GetHooks();
			if (!self || !hooks || !hooks->Network) return;

			const RuptureWavePacket p = Encode(s, ++g_sequence);
			Network::SendPacketToAllClients(hooks, self, p);

			std::lock_guard<std::mutex> lk(g_mutex);
			++g_display.packetsSent;
			g_display.sequence = g_sequence;
		}

		void OnWavePacket(const RuptureWavePacket& p)
		{
			if (p.schemaVersion != kSchemaVersion)
			{
				LOG_WARN("Dropped wave packet with schema %u (expected %u) — server and client plugin versions differ",
					p.schemaVersion, kSchemaVersion);
				return;
			}

			std::lock_guard<std::mutex> lk(g_mutex);
			g_display.snapshot   = Decode(p);
			g_display.sequence   = p.sequence;
			g_display.hasData    = true;
			g_display.stale      = false;
			g_display.waitReason = WaitReason::None;
			++g_display.packetsReceived;
			g_lastPacketTime = NowSeconds();
		}

		void OnHelloPacket(void* senderPC, const RuptureHelloPacket& p)
		{
			if (p.schemaVersion != kSchemaVersion) return;
			WaveNet::SendSnapshotTo(senderPC);
		}

		void SendHello()
		{
			auto* self  = GetSelf();
			auto* hooks = GetHooks();
			if (!self || !hooks || !hooks->Network) return;

			const RuptureHelloPacket hello{ kSchemaVersion, { 0, 0, 0 } };
			Network::SendPacketToServer(hooks, self, hello);
			g_helloRetryCooldown = kHelloRetrySeconds;
		}

		// Authority side. This is the first moment a packet to this client is not
		// thrown away; the loader neither buffers nor replays, so the join-time
		// snapshot has to be sent from here and from nowhere earlier.
		void OnClientReady(void* playerController)
		{
			if (!playerController) return;

			{
				std::lock_guard<std::mutex> lk(g_mutex);
				if (std::find(g_readyClients.begin(), g_readyClients.end(), playerController) == g_readyClients.end())
					g_readyClients.push_back(playerController);
				g_display.readyClients = static_cast<uint32_t>(g_readyClients.size());
			}

			// Outside the lock — the send path takes the same non-recursive mutex.
			WaveNet::SendSnapshotTo(playerController);
		}

		void OnPlayerLeft(void* exitingController)
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			g_readyClients.erase(
				std::remove(g_readyClients.begin(), g_readyClients.end(), exitingController),
				g_readyClients.end());
			g_display.readyClients = static_cast<uint32_t>(g_readyClients.size());
		}

		// Client side. Until this fires, nothing we send can arrive.
		void OnServerReady(const char* serverBuildTag)
		{
			LOG_INFO("Server link ready (authority loader %s)",
				serverBuildTag && serverBuildTag[0] ? serverBuildTag : "unknown");

			g_serverReady = true;
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				g_display.serverLinkReady = true;
			}

			// The authority pushes a snapshot from its own client-ready callback,
			// so this is belt and braces: it also covers an authority whose copy of
			// the plugin came up without ever seeing us become ready.
			SendHello();
		}

		// Which readiness signal matters depends on which side of the wire we turn
		// out to be, and that is not known until the net mode resolves.
		void EnsureReadinessWiring()
		{
			auto* self  = GetSelf();
			auto* hooks = GetHooks();
			if (!self || !hooks || !hooks->Network) return;

			if (RoleHasAuthority(g_role))
			{
				// Standalone has no connections to be ready, and registering there
				// only earns a "not the authority" warning from the loader.
				if (g_role == SessionRole::Solo || g_clientReadyRegistered) return;

				hooks->Network->RegisterClientReadyCallback(self, &OnClientReady);
				if (hooks->Players)
					hooks->Players->RegisterOnPlayerLeft(&OnPlayerLeft);
				g_clientReadyRegistered = true;
			}
			else if (g_role == SessionRole::RemoteClient && !g_serverReadyRegistered)
			{
				hooks->Network->RegisterServerReadyCallback(self, &OnServerReady);
				g_serverReadyRegistered = true;
			}
		}

		void ClearReadinessWiring()
		{
			auto* self  = GetSelf();
			auto* hooks = GetHooks();
			if (self && hooks && hooks->Network)
			{
				if (g_clientReadyRegistered)
				{
					hooks->Network->UnregisterClientReadyCallback(self, &OnClientReady);
					if (hooks->Players)
						hooks->Players->UnregisterOnPlayerLeft(&OnPlayerLeft);
				}
				if (g_serverReadyRegistered)
					hooks->Network->UnregisterServerReadyCallback(self, &OnServerReady);
			}
			g_clientReadyRegistered = false;
			g_serverReadyRegistered = false;
		}
	}

	void WaveNet::Initialize()
	{
		auto* self  = GetSelf();
		auto* hooks = GetHooks();
		if (!self || !hooks || !hooks->Network)
		{
			LOG_WARN("Network channel unavailable — wave data will not be synchronised");
			return;
		}

#if defined(MODLOADER_SERVER_BUILD)
		g_helloHandler = Network::OnServerReceive<RuptureHelloPacket>(hooks, self, &OnHelloPacket);
#else
		g_waveHandler = Network::OnReceive<RuptureWavePacket>(hooks, self, &OnWavePacket);
		// A listen host is also a server, so it answers hello packets too.
		g_helloHandler = Network::OnServerReceive<RuptureHelloPacket>(hooks, self, &OnHelloPacket);
#endif
	}

	void WaveNet::Shutdown()
	{
		ClearReadinessWiring();

		auto* self  = GetSelf();
		auto* hooks = GetHooks();
		if (self && hooks && hooks->Network)
		{
			if (g_waveHandler)
				hooks->Network->UnregisterMessageHandler(self, typeid(RuptureWavePacket).name(), g_waveHandler);
			if (g_helloHandler)
				hooks->Network->UnregisterServerMessageHandler(self, typeid(RuptureHelloPacket).name(), g_helloHandler);
		}
		g_waveHandler  = nullptr;
		g_helloHandler = nullptr;
	}

	void WaveNet::ResetSession()
	{
		WaveReader::InvalidateCache();

		// Readiness is per-connection, so none of it survives a world change. The
		// next tick re-resolves the role and re-registers what that role needs.
		ClearReadinessWiring();

		g_role               = SessionRole::Unresolved;
		g_broadcastAccum     = 0.0f;
		g_pollAccum          = 0.0f;
		g_serverReady        = false;
		g_helloRetryCooldown = 0.0f;
		g_hasSentAnything    = false;
		g_lastSentSnapshot   = WaveSnapshot{};
		g_lastPacketTime     = 0.0;

		std::lock_guard<std::mutex> lk(g_mutex);
		g_readyClients.clear();
		g_display            = DisplayState{};
		g_display.waitReason = WaitReason::DetectingSession;
	}

	void WaveNet::SendSnapshotTo(void* playerController)
	{
		auto* self  = GetSelf();
		auto* hooks = GetHooks();
		if (!self || !hooks || !hooks->Network || !playerController) return;
		if (!RoleHasAuthority(g_role) || !g_hasSentAnything) return;

		// The same predicate SendPacketToClient gates on. Checking it here keeps
		// the sent counter honest instead of counting packets the loader drops.
		if (!hooks->Network->IsClientReady(playerController, self)) return;

		const RuptureWavePacket p = Encode(g_lastSentSnapshot, g_sequence);
		Network::SendPacketToPlayer(hooks, self, playerController, p);

		std::lock_guard<std::mutex> lk(g_mutex);
		++g_display.packetsSent;
	}

	void WaveNet::Tick(float deltaSeconds)
	{
		auto* hooks = GetHooks();
		if (!hooks) return;

		// Net mode only resolves once an actor exists, so keep asking until it does
		// rather than assuming a role and drawing the wrong thing.
		if (g_role == SessionRole::Unresolved && hooks->NetMode)
		{
			const SessionRole resolved = RoleFromNetMode(hooks->NetMode->GetNetMode());
			if (resolved != SessionRole::Unresolved)
			{
				g_role = resolved;
				LOG_INFO("Session role resolved: %s", RoleName(g_role));
				EnsureReadinessWiring();
			}
		}

		{
			std::lock_guard<std::mutex> lk(g_mutex);
			g_display.role = g_role;
		}

		if (g_role == SessionRole::Unresolved)
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			g_display.hasData    = false;
			g_display.waitReason = WaitReason::DetectingSession;
			return;
		}

		const Settings& cfg = Config::Get();

		if (RoleHasAuthority(g_role))
		{
			// The broadcast clock runs on real time; the poll clock is separate so
			// ProcessEvent traffic stays bounded regardless of frame rate.
			g_broadcastAccum += deltaSeconds;

			g_pollAccum += deltaSeconds;
			if (g_pollAccum < kPollIntervalSeconds)
				return;
			g_pollAccum = 0.0f;

			WaveSnapshot snapshot;
			WaitReason   reason = WaitReason::None;
			const bool   ok     = WaveReader::Poll(snapshot, reason);

			{
				std::lock_guard<std::mutex> lk(g_mutex);
				if (ok)
				{
					g_display.snapshot   = snapshot;
					g_display.hasData    = true;
					g_display.stale      = false;
					g_display.dataAgeSecs = 0.0f;
					g_display.waitReason = WaitReason::None;
				}
				else
				{
					g_display.hasData    = false;
					g_display.waitReason = reason;
				}
			}

			if (!ok) return;

			// Solo has nobody to tell.
			if (g_role == SessionRole::Solo) { g_lastSentSnapshot = snapshot; g_hasSentAnything = true; return; }

			const bool dueByTime  = g_broadcastAccum >= cfg.broadcastInterval;
			const bool dueByEvent = !g_hasSentAnything || IsNotableTransition(g_lastSentSnapshot, snapshot);

			if (dueByTime || dueByEvent)
			{
				if (dueByEvent && g_hasSentAnything && cfg.verboseLogging)
				{
					LOG_DEBUG("Wave transition: %s(%u) -> %s(%u), broadcasting immediately",
						StageName(g_lastSentSnapshot.stage), g_lastSentSnapshot.substage,
						StageName(snapshot.stage), snapshot.substage);
				}
				Broadcast(snapshot);
				g_lastSentSnapshot = snapshot;
				g_hasSentAnything  = true;
				g_broadcastAccum   = 0.0f;
			}
			return;
		}

		// ---- Remote client -----------------------------------------------------
		if (g_helloRetryCooldown > 0.0f)
			g_helloRetryCooldown -= deltaSeconds;

		// Decide under the lock, send outside it. A handler reached by the send
		// would try to take the same non-recursive mutex on this thread.
		bool nudgeServer = false;
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			g_display.serverLinkReady = g_serverReady;

			// Before the authority acknowledges us there is no link to be quiet:
			// anything we send is dropped, and the silence is not the server's
			// fault yet. Saying so beats blaming it for having no data.
			if (!g_serverReady)
			{
				g_display.waitReason = WaitReason::LinkHandshake;
				return;
			}

			if (!g_display.hasData)
			{
				g_display.waitReason = WaitReason::NoServerData;

				// A hello is how an authority that loaded the plugin after we
				// joined finds out we want data. This retry used to live only in
				// the stale branch below, which needs data we have never had — so
				// a lost first hello was never retried at all.
				nudgeServer = g_helloRetryCooldown <= 0.0f;
			}
			else
			{
				g_display.dataAgeSecs = static_cast<float>(NowSeconds() - g_lastPacketTime);
				if (g_display.dataAgeSecs > cfg.staleAfterSeconds)
				{
					g_display.stale      = true;
					g_display.waitReason = WaitReason::LinkStale;

					// Nudge the server occasionally in case it restarted.
					nudgeServer = g_helloRetryCooldown <= 0.0f;
				}
				else
				{
					g_display.stale      = false;
					g_display.waitReason = WaitReason::None;
				}
			}
		}

		if (nudgeServer)
			SendHello();
	}

	DisplayState WaveNet::GetDisplayState()
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		DisplayState d = g_display;

		if (!d.hasData) return d;

		// Clients tick the countdown down between packets so it reads smoothly.
		// Turning this off shows only what the server actually sent, which will
		// visibly step once per broadcast interval.
		const Settings& cfg = Config::Get();
		if (d.role == SessionRole::RemoteClient && cfg.countdownInterp && !d.stale)
		{
			const float age = static_cast<float>(NowSeconds() - g_lastPacketTime);
			if (age > 0.0f && age < 60.0f)
			{
				auto advance = [age](float& v, bool valid) {
					if (!valid) return;
					v = v > age ? v - age : 0.0f;
				};
				advance(d.snapshot.stageRemaining, d.snapshot.stageTimeValid);
				advance(d.snapshot.substageRemaining, d.snapshot.substageTimeValid);
				advance(d.snapshot.nextWaveIn, d.snapshot.nextWaveTimeValid);

				if (d.snapshot.stageTimeValid && d.snapshot.stageDuration > 0.0f)
				{
					const float p = 1.0f - (d.snapshot.stageRemaining / d.snapshot.stageDuration);
					d.snapshot.stageProgress = p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
				}
				d.snapshot.timeSinceLastWave += age;
			}
		}

		return d;
	}
}
