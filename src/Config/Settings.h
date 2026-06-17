#pragma once

// Persistent user settings, read once at startup from Data/OSF/settings.json. These are the
// user's safety toggles, and they win over what a scene asks for: a mechanism only runs when
// both the scene wants it and the user allows it. A disabled mechanism makes its osf.* action
// quietly skip (no failure event); cleanup and undo always run regardless.
//
// The file is optional (absent = compiled defaults) and owned by the user: the build doesn't
// ship one, so people keep theirs across updates. The pack and scene scans skip the reserved
// "settings.json" filename.
//
// Recognized keys (flat; unknown keys warn):
//   {
//     "soundEnabled": true,       // SoundService (osf.voice.play + the sound lane)
//     "soundVolume": 1.0,         // master cue volume (clamped 0..2)
//     "fadeEnabled": true,        // FadeService (osf.fade.out/in)
//     "equipmentEnabled": true    // EquipmentService (osf.equipment.hide) — alias: "undressEnabled"
//   }
// // comments are tolerated, like pack JSON.

namespace OSF::Config::Settings
{
	// Reads and applies Data/OSF/settings.json. Missing file = silent defaults; a malformed
	// file logs and applies nothing (fail-soft).
	void Load();
}
