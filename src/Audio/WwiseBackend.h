#pragma once

// Engine-native sound playback: posts events through the game's own statically
// linked Wwise engine (AK::SoundEngine::PostEvent), so the sound rides the game
// mix — volume sliders, pause, ducking and busses all apply. Cue "sound" entries
// of the form "event:<WwiseEventName>" or "event:0x<akEventID>" route here; plain
// file paths keep playing through the miniaudio device (SoundService). Wwise can
// only fire events that already exist in the game's loaded soundbanks, not loose
// files, so it's the opt-in path for sounds the game itself ships.
//
// For now, events post on the PLAYER's Wwise game object (the engine's special-case
// ID 2), i.e. at the listener, with no per-actor 3D source. The per-refr resolver
// wants an engine-built audio-space key object we can't safely synthesize yet, so
// positioned posting is left for later. In practice scene cues fire near the player,
// so the difference is attenuation rather than correctness.
//
// Every engine address is prologue-checked before we call it (the bytes are the same
// on 1.16.242 and 1.16.244); on a mismatch we disable event cues, log once, and leave
// file-path cues alone.
//
// PostEvent only enqueues into AK's command queue and is safe to call from any thread,
// so firing cues from the animation job threads needs no marshaling.

namespace OSF::Audio::Wwise
{
	// A reimplementation of AK::SoundEngine::GetIDFromString: FNV-1 32-bit over the
	// lowercased name (basis 0x811C9DC5, prime 0x1000193), matching the engine's own
	// hasher exactly. Lets packs name events directly, with no soundbank metadata lookup.
	std::uint32_t HashEventName(std::string_view a_name);

	// Recognizes the "event:" cue-sound spec. Returns the AkUniqueID (hex
	// literal taken verbatim, anything else hashed), or nullopt when a_spec is
	// not an event reference (i.e. it is a file path / "$pool" ref).
	std::optional<std::uint32_t> ParseEventSpec(std::string_view a_spec);

	// True when AK::SoundEngine::PostEvent's prologue matches the verified
	// bytes on this runtime. Computed once; a mismatch logs and stays false.
	bool Available();

	// Fire-and-forget post on the player's game object. Returns the
	// AkPlayingID (0 = the engine rejected it — usually an event ID that is
	// not in any loaded bank). Safe from any thread; no-op when !Available().
	std::uint32_t PostEvent(std::uint32_t a_eventID);
}
