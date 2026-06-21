#pragma once

// Persistent user settings, read once at startup from Data/OSF/settings.json. 
// These win over what a scene asks for: a mechanism only runs when both the scene wants it and the user allows it. 
// A disabled mechanism makes its osf.* action quietly skip (no failure event); cleanup and undo always run regardless.

// Recognized keys (flat; unknown keys warn):
//   {
//     "soundEnabled": true,       // SoundService (osf.voice.play + the sound lane)
//     "soundVolume": 1.0,         // master cue volume (clamped 0..2)
//     "fadeEnabled": true,        // FadeService (osf.fade.out/in)
//     "equipmentEnabled": true,   // EquipmentService (osf.equipment.hide), alias: "undressEnabled"
//     "weaponEnabled": true,      // WeaponService (osf.weapon.sheathe)
//   }

namespace OSF::Config::Settings
{
	void Load();
}
