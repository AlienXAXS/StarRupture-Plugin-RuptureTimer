#include "debug/wave_actions.h"

#if !defined(RUPTURETIMER_DEBUG_TOOLS)

namespace RuptureTimer
{
	void WaveActions::Request(Action) {}
	void WaveActions::PumpPendingAction() {}
	bool WaveActions::IsAvailable() { return false; }
}

#else

#include "plugin_helpers.h"

#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"

#include <atomic>

namespace RuptureTimer
{
	namespace
	{
		std::atomic<WaveActions::Action> g_pending{ WaveActions::Action::None };

		SDK::ACrPlayerControllerBase* LocalController()
		{
			SDK::UWorld* world = SDK::UWorld::GetWorld();
			if (!world) return nullptr;

			SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
			if (!pc || !pc->IsA(SDK::ACrPlayerControllerBase::StaticClass()))
				return nullptr;

			return static_cast<SDK::ACrPlayerControllerBase*>(pc);
		}

		const char* ActionName(WaveActions::Action a)
		{
			switch (a)
			{
			case WaveActions::Action::StartHeatWave:   return "ServerDebugStartResumeDefaultHeatWave";
			case WaveActions::Action::SkipToNextStage: return "ServerDebugSkipToNextWaveStage";
			case WaveActions::Action::PauseWave:       return "ServerDebugPauseWave";
			case WaveActions::Action::CancelWave:      return "ServerDebugCancelWave";
			default:                                   return "None";
			}
		}
	}

	void WaveActions::Request(Action action)
	{
		g_pending.store(action, std::memory_order_release);
	}

	bool WaveActions::IsAvailable()
	{
		return LocalController() != nullptr;
	}

	void WaveActions::PumpPendingAction()
	{
		const Action action = g_pending.exchange(Action::None, std::memory_order_acq_rel);
		if (action == Action::None) return;

		SDK::ACrPlayerControllerBase* pc = LocalController();
		if (!pc)
		{
			LOG_WARN("WaveActions: no local ACrPlayerControllerBase, cannot send %s",
				ActionName(action));
			return;
		}

		LOG_INFO("WaveActions: sending %s", ActionName(action));

		switch (action)
		{
		case Action::StartHeatWave:   pc->ServerDebugStartResumeDefaultHeatWave(); break;
		case Action::SkipToNextStage: pc->ServerDebugSkipToNextWaveStage();        break;
		case Action::PauseWave:       pc->ServerDebugPauseWave();                  break;
		case Action::CancelWave:      pc->ServerDebugCancelWave();                 break;
		default: break;
		}
	}
}

#endif // RUPTURETIMER_DEBUG_TOOLS
