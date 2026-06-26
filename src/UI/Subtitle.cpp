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
		// Default subtitle hold when an entry doesn't set `duration`. Seconds.
		constexpr float kDefaultHoldSecs = 4.0f;

		// Engine subtitle box, runtime-proven on 1.16.244 (osf-re ui.subtitle, 2026-06-26 — probe
		// `sub spk GLORP HELLOWORLD` rendered "GLORP: HELLOWORLD" live, pid 51396).
		//
		// There is NO SubtitleManager::ShowSubtitle method: the box is shown by Notify()ing the engine's
		// ShowSubtitleEvent source and cleared by Notify()ing HideSubtitleEvent — the same event-source
		// idiom UI::HudMessage uses for HUD popups. We resolve the two GetEventSource accessors by AddrLib
		// ID and drive them through CommonLibSF's BSTEventSource<T>::Notify (which builds the engine's
		// NotifyVisitor and calls BSTEventSource::Notify 123824). Self-contained on purpose, like
		// UI/FadeService.cpp + Audio/WwiseBackend.cpp: the binding lives here, not in a CLSF header.
		//
		// CommonLibSF previously left these accessors {0}, mislabelled 133631/133630 (those are unrelated
		// tagged-object accessors — the label was drift). The proven IDs are 86874 / 86875.
		constexpr REL::ID kShowSubtitleSource{ 86874 };  // ShowSubtitleEvent::Event::GetEventSource (0x141495c90)
		constexpr REL::ID kHideSubtitleSource{ 86875 };  // HideSubtitleEvent::Event::GetEventSource (0x141495d00)

		// ShowSubtitleEvent payload (sizeof 0x18). Fields are RAW UTF-8 `const char*`, NOT BSFixedString:
		// the sink (HUDSubtitleDataModel::ProcessEvent, 86881) runs its own byte-wise UTF-8 converter, so a
		// BSFixedString entry pointer renders on-screen mojibake. Field order + type were runtime-corrected
		// in osf-re (the static read had speaker/text reversed and assumed BSFixedString).
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

		// HideSubtitleEvent payload is empty (Notify clears the box).
		struct HideEvent
		{
		};

		using GetShowSourceFn = RE::BSTEventSource<ShowEvent>* (*)();
		using GetHideSourceFn = RE::BSTEventSource<HideEvent>* (*)();

		// Steady-clock ms deadline at which the shown line should be hidden; 0 = nothing shown.
		std::atomic<std::int64_t> g_hideAtMs{ 0 };

		std::int64_t NowMs()
		{
			using namespace std::chrono;
			return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
		}

		// Resolve the show-source accessor once. An unresolved ID (id()==0 on an off-version runtime) leaves
		// the native box disabled; Show() then falls back to the HUD channel. The accessor is a nullary
		// "return the global event source" function (the most stable shape there is), so AddrLib resolution
		// plus the null-source check below is the meaningful guard — no signature to drift.
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

		// Resolve the speaker's display name for the line prefix (GetDisplayFullName, REFR vtable 0xF2).
		const char* speakerName = a_speaker ? a_speaker->GetDisplayFullName() : nullptr;

		REX::DEBUG("[UI] subtitle [{}] \"{}\" ({:.1f}s)",
			(speakerName && speakerName[0]) ? speakerName : "<narrator>", a_text, holdSecs);

		auto* src = ShowSource();
		if (!src) {
			// Native subtitle box unavailable on this runtime (ID unresolved, or the event source isn't up
			// yet) — fall back to the HUD channel so the line is never lost. Reads as "Name — text".
			std::string line;
			if (speakerName && speakerName[0] != '\0') {
				line.append(speakerName).append(" \xE2\x80\x94 ");  // em dash
			}
			line.append(a_text);
			HudMessage::Show(line);
			return;
		}

		// Notify is synchronous and the sink copies the text out (UTF-8 conversion) during the call, so a
		// local buffer that outlives the Notify is enough.
		const std::string text{ a_text };
		ShowEvent ev{};
		ev.subtitleText = text.c_str();
		ev.speakerName  = speakerName ? speakerName : "";
		ev.isPlayer     = (a_speaker != nullptr && a_speaker == RE::PlayerCharacter::GetSingleton());
		src->Notify(ev);

		// Arm the hide deadline. max(): a later line only ever EXTENDS the on-screen hold, never shortens a
		// pending hide. A new Show replaces the visible line; Tick() hides the last one when its hold ends.
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
			return;  // another thread won the hide (or a new line moved the window)
		}
		PostHide();
	}

	void OnStopAll()
	{
		// Hide right now if a line is showing: StopAll runs synchronously in the save-load sink, and a
		// subtitle left in the box must not bleed into the loaded world. Notify only enqueues onto the
		// engine's data model, so it's safe in the pre-load window.
		if (g_hideAtMs.exchange(0, std::memory_order_relaxed) != 0) {
			PostHide();
		}
	}
}
