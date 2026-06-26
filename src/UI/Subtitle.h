#pragma once

// Spoken-line subtitle ("the dialogue box"). A "voice" line is just a normal sound clip that carries
// text: a clip authored with subtitle text in its *.sounds.json pool renders that text here whenever
// it plays (SceneRuntime::PlaySound -> SoundRegistry::TextForClip -> Show), so audio + box are driven
// from one place with no separate lane — see docs/SCENE_SCHEMA.md.
//
// TWO-PHASE by design (the audio half is already engine-native; the box is the open RE):
//
//  * PHASE 1 (now): a content-neutral seam. Show() logs the line and posts it through the engine HUD
//    message channel (UI::HudMessage) so the data-driven pipeline is visible and testable end-to-end
//    immediately and NEVER crashes. This is a placeholder *renderer*, not the vanilla subtitle box.
//
//  * PHASE 2 (OSF RE): swap Show()'s body for the engine's own subtitle UI so the line renders in the
//    real dialogue/subtitle box, positioned on the speaker. The injection point is unproven RE today.
//    Leads traced statically on 1.16.244 (see Subtitle.cpp for the full note):
//      - SubtitleManager vtable       AddrLib 460938 -> 0x144d0c408
//      - HUDSubtitleDataModel vtable  AddrLib 437283 -> 0x144c5ed68  (the Flash data shuttle)
//      - candidate show/dispatch fn   AddrLib 133631 -> 0x1426749b0  (CLSF mislabels this a magic-static
//                                     event accessor; it is actually a `this`-method, currently {0}/unbound)
//    The swap is a one-file change behind a FadeService-style prologue guard (AddrLib id + verified
//    prologue bytes, self-disabling on a future patch); Tick()/Clear() get wired then for expiry.
//
// Threading: Show() is called from the scene dispatch (job threads, under the scene lock) and must not
// block. The HUD post only enqueues an engine event (any-thread-safe); resolving the speaker name is a
// cheap read of the ref. Cheap no-op when text is empty.

#include <string_view>

namespace RE
{
	class Actor;
}

namespace OSF::UI::Subtitle
{
	// Show a spoken line `a_text` attributed to `a_speaker` (nullptr = unattributed) for `a_seconds`
	// (<= 0 selects a default hold). Safe from any thread; a no-op on empty text.
	void Show(RE::Actor* a_speaker, std::string_view a_text, float a_seconds = 0.0f);
}
