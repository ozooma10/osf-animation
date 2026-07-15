#pragma once

// Discoverability nudge for the scene browser: until the player has opened it a few times,
// each save load fires a HUD popup saying the menu exists (default F10). State persists
// per-install (not per-save) in <Documents>\My Games\Starfield\OSF\first-run.json —
// knowing the hotkey is player knowledge, so a new save shouldn't reset it.

namespace OSF::UI::FirstRunHint
{
	// A save just finished loading (TESLoadGameEvent backstop, game main thread). Shows the
	// hint unless the player has opened the browser enough times, the show budget is spent,
	// the setting is off, or OSF UI is absent (F10 opens nothing then — a hint would lie).
	void OnPostLoad();

	// The scene-browser view reported it became visible (osf.opened command, game main thread).
	void OnMenuOpened();

	// OSF UI settings menu, osf "firstRunHint" (default on; src/API/UISettings.cpp).
	void SetEnabled(bool a_on);
}
