#pragma once

// Engine-native cue playback: posts events through the game's own statically
// linked Wwise sound engine (AK::SoundEngine::PostEvent), so the sound rides
// the game mix — volume sliders, pause, ducking, busses all apply. This is
// the opt-in upgrade path mapped by the Wwise RE round (wwise-audio-re-
// handoff.md): cue "sound" entries of the form "event:<WwiseEventName>" or
// "event:0x<akEventID>" route here; plain file paths keep playing through
// the miniaudio device (SoundService) — Wwise can only fire events that
// exist in the game's loaded soundbanks, not loose files.
//
// v1 limitation (deliberate): events post on the PLAYER's Wwise game object
// (engine special-case ID 2), i.e. at the listener — no per-actor 3D source.
// The per-refr game-object resolver (AddrLib 73392) takes an engine-built
// audio-space key object we cannot safely synthesize yet; positioned posting
// is the open follow-up in the handoff. Scene cues fire near the player in
// practice, so the audible difference is attenuation, not correctness.
//
// Verify-before-call: every engine address is prologue-gated (runtime-proven
// bytes, identical 1.16.242/1.16.244); a mismatch self-disables event cues
// and logs once — file-path cues are unaffected.
//
// Thread-safety: PostEvent is enqueue-only into AK's command queue and was
// runtime-proven callable from arbitrary non-game threads (probe round 2),
// so cue firing from animation job threads needs no marshaling.

namespace OSF::Audio::Wwise
{
	// AK::SoundEngine::GetIDFromString reimplemented: FNV-1 32-bit over the
	// lowercased name (basis 0x811C9DC5, prime 0x1000193) — RE-verified exact
	// against the engine's own hasher (AddrLib 150371). Lets packs name events
	// directly; no WWED form or soundbank metadata lookup needed.
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
