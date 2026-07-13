#pragma once

// Persistent user settings, read once at startup from Data/OSF/settings.json.
// Whether a mechanism runs is decided by the scene/API alone — settings only tune behaviour,
// they do not gate features on or off.

// Recognized keys (flat; unknown keys warn):
//   {
//     "logLevel": "info",            // trace|debug|info|warn|error.
//     "debugNotifications": false,    // top-right HUD popup on each stage transition (dev/test; default off)
//     "firstRunHint": true,           // "press F10" HUD popup on load until the scene browser has been opened 3 times (default on)
//     "hotkeys": {                    // always-on global hotkeys: "KeyName": "command[:arg]" (default: none)
//       "B": "openBrowser",                 // open the scene browser (like the Data Slate)
//       "G": "openWheel",                   // emote wheel (optional :tagPrefix, default player.emote.)
//       "N": "toggleSceneTags:player.sit"   // start a scene matching the tags / end it on re-press
//     }
//     // Key names: F1-F24, A-Z, 0-9, Numpad0-9, Space, Tab, Enter, Backspace, Insert, Delete,
//     // Home, End, PageUp, PageDown, Up/Down/Left/Right, Minus, Equals, Comma, Period, Slash,
//     // Backslash, Semicolon, Apostrophe, LeftBracket, RightBracket, Grave — or "0x2D"-style
//     // hex VK codes. No modifier combos. Keys the scene director channel uses
//     // (Space, P, End, 0, +, -, E) are refused. Bound keys are NOT consumed: the game still
//     // sees the press, so avoid keys the game itself uses heavily.
//   }

namespace OSF::Config::Settings
{
	void Load();
}
