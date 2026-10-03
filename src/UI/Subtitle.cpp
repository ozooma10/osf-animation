#include "UI/Subtitle.h"

#include "UI/HudMessage.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>

namespace OSF::UI::Subtitle
{
	namespace
	{
		constexpr float kDefaultHoldSecs = 4.0f;

		// ShowSubtitleEvent / HideSubtitleEvent GetEventSource accessors (CLSF leaves them {0}), driven through
		// BSTEventSource<T>::Notify. Not 133631/133630: those are unrelated accessors.
		constexpr REL::ID kShowSubtitleSource{ 86874 };  // 0x141495c90
		constexpr REL::ID kHideSubtitleSource{ 86875 };  // 0x141495d00

		// sizeof 0x18. Raw UTF-8 `const char*`, not BSFixedString: the sink (86881) converts bytes itself, so BSFixedString renders mojibake.
		struct ShowEvent
		{
			const char* subtitleText = nullptr;  // +0x00
			const char* speakerName  = nullptr;  // +0x08
			bool        isPlayer     = false;    // +0x10
		};
		static_assert(sizeof(ShowEvent) == 0x18);
		static_assert(offsetof(ShowEvent, subtitleText) == 0x00);
		static_assert(offsetof(ShowEvent, speakerName) == 0x08);
		static_assert(offsetof(ShowEvent, isPlayer) == 0x10);

		// Payload is empty.
		struct HideEvent
		{
		};

		using GetShowSourceFn = RE::BSTEventSource<ShowEvent>* (*)();
		using GetHideSourceFn = RE::BSTEventSource<HideEvent>* (*)();

		// Steady-clock ms; 0 = nothing shown.
		std::atomic<std::int64_t> g_hideAtMs{ 0 };

		std::int64_t NowMs()
		{
			using namespace std::chrono;
			return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
		}

		// Unresolved (off-version) leaves the native box disabled; Show() falls back to the HUD channel.
		RE::BSTEventSource<ShowEvent>* ShowSource()
		{
			static const GetShowSourceFn get = (kShowSubtitleSource.id() != 0)
			                                       ? reinterpret_cast<GetShowSourceFn>(kShowSubtitleSource.address())
			                                       : nullptr;
			return get ? get() : nullptr;
		}

		RE::BSTEventSource<HideEvent>* HideSource()
		{
			static const GetHideSourceFn get = (kHideSubtitleSource.id() != 0)
			                                       ? reinterpret_cast<GetHideSourceFn>(kHideSubtitleSource.address())
			                                       : nullptr;
			return get ? get() : nullptr;
		}

		void PostHide()
		{
			if (auto* src = HideSource()) {
				HideEvent ev{};
				src->Notify(ev);
			}
		}
	}

	void Show(RE::Actor* a_speaker, std::string_view a_text, float a_seconds)
	{
		if (a_text.empty()) {
			return;
		}
		const float holdSecs = a_seconds > 0.0f ? a_seconds : kDefaultHoldSecs;

		// GetDisplayFullName, REFR vtable 0xF2.
		const char* speakerName = a_speaker ? a_speaker->GetDisplayFullName() : nullptr;

		REX::DEBUG("[UI] subtitle [{}] \"{}\" ({:.1f}s)",
			(speakerName && speakerName[0]) ? speakerName : "<narrator>", a_text, holdSecs);

		auto* src = ShowSource();
		if (!src) {
			// Native box unavailable (or source not up yet): fall back to the HUD channel.
			std::string line;
			if (speakerName && speakerName[0] != '\0') {
				line.append(speakerName).append(" \xE2\x80\x94 ");  // em dash
			}
			line.append(a_text);
			HudMessage::Show(line);
			return;
		}

		// Notify is synchronous and the sink copies the text, so a local buffer suffices.
		const std::string text{ a_text };
		ShowEvent ev{};
		ev.subtitleText = text.c_str();
		ev.speakerName  = speakerName ? speakerName : "";
		ev.isPlayer     = (a_speaker != nullptr && a_speaker == RE::PlayerCharacter::GetSingleton());
		src->Notify(ev);

		// max(): a later line only extends the hold.
		const std::int64_t deadline = NowMs() + static_cast<std::int64_t>(holdSecs * 1000.0f);
		std::int64_t current = g_hideAtMs.load(std::memory_order_relaxed);
		while (deadline > current &&
		       !g_hideAtMs.compare_exchange_weak(current, deadline, std::memory_order_relaxed)) {
		}
	}

	void Tick()
	{
		std::int64_t deadline = g_hideAtMs.load(std::memory_order_relaxed);
		if (deadline == 0 || NowMs() < deadline) {
			return;
		}
		if (!g_hideAtMs.compare_exchange_strong(deadline, 0, std::memory_order_relaxed)) {
			return;
		}
		PostHide();
	}

	void OnStopAll()
	{
		// A line must not bleed into the loaded world; Notify only enqueues, so it's safe pre-load.
		if (g_hideAtMs.exchange(0, std::memory_order_relaxed) != 0) {
			PostHide();
		}
	}
}
