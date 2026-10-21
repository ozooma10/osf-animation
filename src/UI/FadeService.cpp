#include "UI/FadeService.h"

#include "API/Health.h"
#include "Util/Hooking.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>

namespace OSF::UI
{
	namespace
	{
		// Engine function behind Papyrus Game.FadeOutGame: posts kShow to "FaderMenu" via UIMessageQueue.
		constexpr REL::ID kFadeScreenPoster{ 114430 };

		// Poster prologue (1.16.242/1.16.244); a mismatch on a future patch disables fades.
		constexpr std::array<std::uint8_t, 32> kFadePosterPrologue{
			0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
			0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57,
			0x48, 0x83, 0xEC, 0x50, 0xC5, 0xF8, 0x29, 0x74, 0x24, 0x40,
			0x41, 0x0F
		};

		// Args after stayFaded mirror the Papyrus thunk: callback=null, setGlobalFadedFlag=1, flags=1.
		using FadeScreenFn = void (*)(bool a_fadingOut, bool a_blackFade, float a_fadeDuration,
			bool a_stayFaded, float a_secsBeforeFade, void* a_doneCallback,
			bool a_setGlobalFadedFlag, std::uint32_t a_flags);

		int64_t NowMs()
		{
			using namespace std::chrono;
			return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
		}
	}

	FadeService& FadeService::GetSingleton()
	{
		static FadeService instance;
		return instance;
	}

	bool FadeService::Available()
	{
		static const bool available = []() {
			if (!Util::Hooking::PrologueMatches(kFadeScreenPoster, kFadePosterPrologue)) {
				REX::WARN("[UI] screen fades disabled: fade poster (ID {}) prologue mismatch on this runtime "
				          "— osf.fade.* actions are no-ops",
					kFadeScreenPoster.id());
				// Tell the player once: a patch moving the anchor is otherwise invisible.
				const nlohmann::json context{
					{ "feature", "screen fades" },
					{ "reason", "engine function signature changed on this game version" },
				};
				API::Health::Report("fade.poster", "fade.disabled",
					API::Health::Severity::kWarning, "screen fades", &context);
				return false;
			}
			REX::DEBUG("[UI] screen fades available: fade poster prologue verified");
			return true;
		}();
		return available;
	}

	void FadeService::PostFade(bool a_fadingOut, float a_fadeSecs, bool a_stayFaded)
	{
		const auto post = reinterpret_cast<FadeScreenFn>(kFadeScreenPoster.address());
		post(a_fadingOut, /*blackFade*/ true, a_fadeSecs, a_stayFaded,
			/*secsBeforeFade*/ 0.0f, /*doneCallback*/ nullptr,
			/*setGlobalFadedFlag*/ true, /*flags*/ 1);
	}

	void FadeService::ScheduleRelease(float a_fadeSecs, float a_holdSecs)
	{
		const auto deadline = NowMs() + static_cast<int64_t>((a_fadeSecs + a_holdSecs) * 1000.0f);
		// max(): a second fade-out only extends the hold.
		int64_t current = releaseAtMs.load(std::memory_order_relaxed);
		while (deadline > current &&
		       !releaseAtMs.compare_exchange_weak(current, deadline, std::memory_order_relaxed)) {
		}
	}

	bool FadeService::FadeToBlack(float a_fadeSecs, float a_holdMaxSecs)
	{
		if (!Available()) {
			return false;
		}
		const float fadeSecs = std::clamp(a_fadeSecs, 0.05f, 5.0f);
		std::lock_guard guard{transitionLock};
		if (transitionOwner) return false;
		// The cap keeps a missed fade.in from leaving the screen black or carrying the latch into a save-load (crash).
		const float holdSecs = std::clamp(a_holdMaxSecs, 0.0f, 10.0f);
		PostFade(/*fadingOut*/ true, fadeSecs, /*stayFaded*/ true);
		ScheduleRelease(fadeSecs, holdSecs);
		return true;
	}

	bool FadeService::FadeFromBlack(float a_fadeSecs)
	{
		if (!Available()) {
			return false;
		}
		std::lock_guard guard{transitionLock};
		if (transitionOwner) return false;
		releaseAtMs.store(0, std::memory_order_relaxed);
		// The engine never refuses a fade-in, even mid-fade-out.
		PostFade(/*fadingOut*/ false, std::clamp(a_fadeSecs, 0.05f, 5.0f), /*stayFaded*/ false);
		return true;
	}

	void FadeService::OnStopAll()
	{
		std::lock_guard guard{transitionLock};
		const bool owned = std::exchange(transitionOwner, 0) != 0;
		// The load path crashes on a held stay-faded latch; posting only enqueues, so it's safe pre-load.
		if ((releaseAtMs.exchange(0, std::memory_order_relaxed) != 0 || owned) && Available()) {
			PostFade(/*fadingOut*/ false, 0.3f, /*stayFaded*/ false);
			REX::DEBUG("[UI] held screen fade released for save-load teardown");
		}
	}

	void FadeService::Tick()
	{
		std::lock_guard guard{transitionLock};
		int64_t deadline = releaseAtMs.load(std::memory_order_relaxed);
		if (deadline == 0 || NowMs() < deadline) {
			return;
		}
		if (!releaseAtMs.compare_exchange_strong(deadline, 0, std::memory_order_relaxed)) {
			return;
		}
		PostFade(/*fadingOut*/ false, 0.5f, /*stayFaded*/ false);
		transitionOwner = 0;
	}
	std::uint64_t FadeService::BeginTransition()
	{
		if (!Available()) return 0;
		std::lock_guard guard{transitionLock};
		if (transitionOwner || releaseAtMs.load() || nextTransition == UINT64_MAX) return 0;
		transitionOwner = ++nextTransition;
		PostFade(true, 0.25f, true);
		// Independent safety net; the handoff cancels at five seconds before this six-second cap.
		releaseAtMs.store(NowMs() + 6000);
		return transitionOwner;
	}
	bool FadeService::OwnsTransition(std::uint64_t token)
	{
		std::lock_guard guard{transitionLock};
		return token && token == transitionOwner;
	}
	bool FadeService::RevealTransition(std::uint64_t token)
	{
		std::lock_guard guard{transitionLock};
		if (!token || token != transitionOwner) return false;
		releaseAtMs.store(0);
		PostFade(false, 0.5f, false);
		return true;
	}
	void FadeService::EndTransition(std::uint64_t token)
	{
		std::lock_guard guard{transitionLock};
		if (!token || token != transitionOwner) return;
		transitionOwner = 0;
		if (releaseAtMs.exchange(0)) PostFade(false, 0.5f, false);
	}
}
