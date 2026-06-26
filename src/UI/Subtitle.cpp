#include "UI/Subtitle.h"

#include "UI/HudMessage.h"

#include <string>

namespace OSF::UI::Subtitle
{
	namespace
	{
		// Default subtitle hold when an entry doesn't set `duration` (the HUD placeholder self-throttles;
		// the value is carried so the PHASE 2 native box has a hold to honour). Seconds.
		constexpr float kDefaultHoldSecs = 4.0f;

		// PHASE 2 — engine subtitle box (NOT YET WIRED; needs an OSF RE runtime-proving pass).
		//
		// The vanilla flow when an NPC speaks: a subtitle string + speaker handle reach the HUD subtitle
		// widget via the HUDSubtitleDataModel -> Flash data shuttle, driven by the SubtitleManager. To
		// render arbitrary text (from JSON) in that box we need ONE of:
		//   (a) SubtitleManager::ShowSubtitle(speakerRef, text, ...) — the direct call (preferred), or
		//   (b) build a ShowSubtitleEvent::Event{speaker, text, ...} and Notify its source.
		//
		// Static leads on 1.16.244 (resolve via OSF RE tools/re/addrlib_query.py):
		//   SubtitleManager vtable        AddrLib 460938  0x144d0c408   (singleton: BSTSingletonSDM static buffer)
		//   HUDSubtitleDataModel vtable   AddrLib 437283  0x144c5ed68
		//   ShowSubtitleEvent::Event vt   AddrLib 437279  0x144c5ed90
		//   candidate dispatch/show fn    AddrLib 133631  0x1426749b0   (a `this`-method on a subtitle/dialogue
		//       object: reads a type byte at +0x28 and a member at +0x10; 28 callers. CommonLibSF labels this
		//       ShowSubtitleEvent::Event::GetEventSource and leaves it {0}/unbound — that label is drift.)
		//   VO voice-path builder         AddrLib 68917   0x140d414b0   (Data\Sound\Voice\<plugin>\<vt>\<formid>.wem;
		//       proven, ui.dialogue_menu / systems.audio_wwise — the speaking-NPC pipeline to trace back from.)
		//
		// When proven, the body below becomes a guarded native call modelled on UI/FadeService.cpp
		// (REL::ID + a verified prologue-byte check that self-disables on a patch); Tick()/Clear() get
		// added for expiry + save-load teardown, wired in GraphManager::Tick and SceneRuntime::StopAll.
		// Keep this file the single place that knows how a subtitle reaches the screen.
	}

	void Show(RE::Actor* a_speaker, std::string_view a_text, float a_seconds)
	{
		if (a_text.empty()) {
			return;
		}
		const float holdSecs = a_seconds > 0.0f ? a_seconds : kDefaultHoldSecs;

		// Resolve the speaker's display name for the line prefix (GetDisplayFullName, REFR vtable 0xF2).
		const char* speakerName = a_speaker ? a_speaker->GetDisplayFullName() : nullptr;

		REX::INFO("Subtitle: [{}] \"{}\" ({:.1f}s)", speakerName ? speakerName : "<narrator>", a_text, holdSecs);

		// PHASE 1 placeholder renderer: the engine HUD-message channel (proven, any-thread-safe). Reads
		// like a line ("Name — text") so authoring is testable now; PHASE 2 swaps this for the real box.
		std::string line;
		if (speakerName && speakerName[0] != '\0') {
			line.append(speakerName).append(" \xE2\x80\x94 ");  // em dash
		}
		line.append(a_text);
		HudMessage::Show(line);
	}
}
