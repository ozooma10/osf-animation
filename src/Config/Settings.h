#pragma once

// Persistent user settings, read once at startup from Data/OSF/settings.json.
// Whether a mechanism runs is decided by the scene/API alone — settings only tune behaviour,
// they do not gate features on or off.

// Recognized keys (flat; unknown keys warn):
//   {
//     "logLevel": "info",            // trace|debug|info|warn|error.
//     "debugNotifications": false,    // top-right HUD popup on each stage transition (dev/test; default off)
//     "firstRunHint": true,           // "press F10" HUD popup on load until the scene browser has been opened 3 times (default on)
//   }

namespace OSF::Config::Settings
{
	void Load();
}
