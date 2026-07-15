#pragma once

// OSF Animation's settings + hotkeys, hosted by OSF UI's MCM platform.
//
// The old Data/OSF/settings.json is GONE: the schema below registers an
// "OSF Animation" card in OSF UI's settings menu (RegisterSettingsSchema,
// bridge MINOR >= 2), values persist in OSF UI's per-mod store, and native
// reaction arrives over SubscribeSettings (replayed once per value on
// subscribe, so boot application is free). Hotkeys are key-typed settings
// dispatched by OSF UI's HotkeyService (SubscribeHotkey, MINOR >= 4) —
// rebindable in-game, conflict-badged, and correctly gated (nothing fires
// while the console is open, a text field is focused, or a rebind capture is
// armed). With OSF UI absent or too old this module degrades loudly to
// defaults: no settings menu, no hotkeys, everything else works.
namespace OSF::API
{
	// Register the schema + subscriptions. Call at kPostDataLoad, after
	// InstallUIBridge() (it fetches the same bridge singleton). Safe no-op
	// (logged) when OSF UI is absent.
	void InstallUISettings();
}
