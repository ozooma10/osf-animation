#pragma once

// Persistent user settings, read once at startup from Data/OSF/settings.json.
// Whether a mechanism runs is decided by the scene/API alone — settings only tune behaviour,
// they do not gate features on or off.

// Recognized keys (flat; unknown keys warn):
//   {
//     "soundVolume": 1.0,            // master cue volume (clamped 0..2)
//     "debugNotifications": true,    // top-right HUD popup on each stage transition (dev/test; default on)
//   }

namespace OSF::Config::Settings
{
	void Load();
}
