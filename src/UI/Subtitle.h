#pragma once

// Spoken-line subtitle ("the dialogue box"). A "voice" line is just a normal sound clip that carries
// text: a clip authored with subtitle text in its *.sounds.json pool renders that text here whenever
// it plays (SceneRuntime::PlaySound -> SoundRegistry::TextForClip -> Show), so audio + box are driven
// from one place with no separate lane — see docs/SCENE_SCHEMA.md.
//
// Renders through the engine's OWN subtitle box (runtime-proven, osf-re ui.subtitle 2026-06-26). The
// engine has NO SubtitleManager::ShowSubtitle method — the box is driven entirely by an event: Show()
// Notify()s a ShowSubtitleEvent (AddrLib 86874) and the box reads "speakerName: text" in the standard
// bottom-of-screen list (it is NOT 3D-positioned on the speaker), rendering regardless of the user's
// subtitle settings (that gate lives upstream of the event). This is the exact event-source idiom
// UI::HudMessage uses for HUD popups. If the subtitle source can't resolve on a given runtime, Show()
// falls back to the HUD-message channel so a line is never silently lost.
//
// EXPIRY / TEARDOWN: the direct-Notify path does NOT auto-hide on a timer (the vanilla producer carries
// the duration; Notify-ing the source directly bypasses it). So Show() arms a hold deadline, Tick()
// Notify()s a HideSubtitleEvent (AddrLib 86875) once it passes, and OnStopAll() hides immediately so a
// line in the box can't bleed into a save-load. Both are wired in GraphManager next to the other
// per-frame services (FadeService/SoundService).
//
// Threading: Show()/Tick() run from the scene dispatch (job threads, under the scene lock) and must not
// block. The event Notify only drives the engine's subtitle data model (any-thread-safe, same as
// UI::HudMessage); resolving the speaker name is a cheap read of the ref. Cheap no-op on empty text.

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

	// Hide the box once a shown line's hold elapses. Rides the update-hook call stream (job threads);
	// atomic early-out when nothing is showing.
	void Tick();

	// Save-load / StopAll teardown: hide any line currently in the box right now. Called synchronously
	// from GraphManager::StopAll.
	void OnStopAll();
}
