#pragma once

// Persistent user settings: Data/OSF/settings.json, read once at kPostDataLoad. These are
// the user/global safety toggles in the settings-precedence rule (SCENE_DESIGN §1.5):
// effective = (scene wants) AND (user setting allows). A disabled mechanism makes its
// osf.* action a SILENT skip (no EVENT_ACTION_FAILED); cleanup/undo always runs regardless.
//
// The file is OPTIONAL (absent = compiled defaults) and user-owned: the build does NOT ship
// one, so MO2 users keep theirs across updates. SceneRegistry/PackRegistry skip the reserved
// filename "settings.json" during scans.
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
