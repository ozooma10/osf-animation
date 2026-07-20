#pragma once

// Discoverability nudge for the scene browser: until the player has opened it a few times,
// each save load fires a HUD popup saying the menu exists (default F10). State persists
// per-install (not per-save) in <Documents>\My Games\Starfield\SFSE\OSF\first-run.json —
// knowing the hotkey is player knowledge, so a new save shouldn't reset it.

namespace OSF::UI::FirstRunHint
{
	// A save just finished loading (TESLoadGameEvent backstop, game main thread). Queues the
	// hint for the first frame the HUD is up — unless the player has opened the browser enough
	// times, the show budget is spent, the setting is off, or OSF UI is absent (F10 opens
	// nothing then — a hint would lie). Only a delivered popup spends a show.
	void OnPostLoad();

	// The view reported it became visible (osf.animation.opened, game main thread).
	// a_browserMode is false when it came up as the emote wheel — that's a different hotkey,
	// so it teaches nothing about the browser and must not count toward retiring the hint.
	void OnMenuOpened(bool a_browserMode);

	// OSF UI settings menu, osf "firstRunHint" (default on; src/API/UISettings.cpp).
	void SetEnabled(bool a_on);
}
