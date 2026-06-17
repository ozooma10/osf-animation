#include "UI/FadeService.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>

namespace OSF::UI
{
	namespace
	{
		// Engine fade poster behind Papyrus Game.FadeOutGame: builds a
		// FaderData and posts kShow to "FaderMenu" through UIMessageQueue.
		// AddrLib ID valid on 1.16.242 (home 0x1ee5970) and 1.16.244
		// (0x1ee2740); osf-re ui.fader_menu, static pass 2026-06-12.
		constexpr REL::ID kFadeScreenPoster{ 114430 };

		// First 32 bytes of the poster, byte-identical on 1.16.242/1.16.244.
		// Mismatch on a future patch = fades self-disable.
		constexpr std::array<std::uint8_t, 32> kFadePosterPrologue{
			0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
			0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57,
			0x48, 0x83, 0xEC, 0x50, 0xC5, 0xF8, 0x29, 0x74, 0x24, 0x40,
			0x41, 0x0F
		};

		// void(bool fadingOut /*cl*/, bool blackFade /*dl*/,
		//      float fadeDuration /*xmm2*/, bool stayFaded /*r9b*/,
		//      float secsBeforeFade, RefCounted* doneCallback,
		//      bool setGlobalFadedFlag, u32 flags) — the recovered signature;
		// trailing constants mirror the Papyrus thunk exactly
		// (callback=null, setGlobalFadedFlag=1, flags=1).
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
			const auto* code = reinterpret_cast<const std::uint8_t*>(kFadeScreenPoster.address());
			if (!code || std::memcmp(code, kFadePosterPrologue.data(), kFadePosterPrologue.size()) != 0) {
				REX::WARN("Screen fades disabled: fade poster (ID {}) prologue mismatch on this runtime "
				          "— osf.fade.* actions are no-ops",
					kFadeScreenPoster.id());
				return false;
			}
			REX::INFO("Screen fades available: fade poster prologue verified");
			return true;
		}();
		return available;
	}

	void FadeService::SetEnabled(bool a_enabled)
	{
		enabled.store(a_enabled, std::memory_order_relaxed);
	}

	bool FadeService::Enabled() const
	{
		return enabled.load(std::memory_order_relaxed);
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
		// max(): a second fade-out only ever extends the hold, never shortens a
		// pending release window.
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
		// The hold cap keeps a scene that never authors a fade.in (and whose
		// undo ledger somehow doesn't run) from leaving the screen black
		// forever — and from carrying the latch into a save-load (the proven
		// crash). The ledger / an authored fade.in normally releases first.
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
		releaseAtMs.store(0, std::memory_order_relaxed);
		// A fade-in always releases the engine-side latch, even mid-fade-out
		// (the engine never refuses a fade-in request).
		PostFade(/*fadingOut*/ false, std::clamp(a_fadeSecs, 0.05f, 5.0f), /*stayFaded*/ false);
		return true;
	}

	void FadeService::OnStopAll()
	{
		// Release right now if anything is held or ramping: StopAll runs synchronously
		// in the save-load sink, and the load path crashes on a held stay-faded latch.
		// Posting only enqueues, so it's safe in the pre-load window.
		if (releaseAtMs.exchange(0, std::memory_order_relaxed) != 0 && Available()) {
			PostFade(/*fadingOut*/ false, 0.3f, /*stayFaded*/ false);
			REX::INFO("Held screen fade released for save-load teardown");
		}
	}

	void FadeService::Tick()
	{
		int64_t deadline = releaseAtMs.load(std::memory_order_relaxed);
		if (deadline == 0 || NowMs() < deadline) {
			return;
		}
		if (!releaseAtMs.compare_exchange_strong(deadline, 0, std::memory_order_relaxed)) {
			return;  // another thread won the release (or the window moved)
		}
		PostFade(/*fadingOut*/ false, 0.5f, /*stayFaded*/ false);
	}
}
