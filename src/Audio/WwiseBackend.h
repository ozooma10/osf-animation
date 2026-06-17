#pragma once

// Engine-native sound playback through the game's own statically linked Wwise engine
// (RE::BGSAudio::AkSoundEngine, the CommonLibSF fork bindings). Everything posted here
// rides the game mix — master/SFX volume sliders, the pause menu, ducking and busses all
// apply — because they are global RTPCs the engine owns.
//
// Two entry shapes, both AK::SoundEngine::PostEvent under the hood:
//
//   1. BAKED EVENTS — cue "sound" entries of the form "event:<WwiseEventName>" or
//      "event:0x<akEventID>". Posts the event with NO external source (cExternals 0). The
//      event must already live in a loaded soundbank. This is the unchanged "event:" path.
//
//   2. EXTERNAL SOURCE (loose modder files) — any event that carries an "External_Source"
//      placeholder slot lets PostEvent substitute caller-supplied media via the trailing
//      (cExternals, AkExternalSourceInfo*) args. This is the GAME'S OWN voice path: it
//      streams loose Data\Sound\Voice\...\<id>.wem files exactly this way (RE-proven on
//      1.16.244, see OSF RE Investigations/Responses/2026-06-17-wwise-external-loose-audio.md).
//      We post loose pack files through the external-source slot of a SHIPPED event that is
//      already resident in a loaded bank ("Dialogue_6_Combat", AkUniqueID 0x5DE4F1F3) — so
//      loose .wav/.wem/.xwm play engine-mixed with NO soundbank authoring and NO LoadBank.
//      (A custom mod .bnk does NOT work: the engine resolves banks by BSResource registry
//      membership, not loose-file presence, so LoadBank on a modder bank returns AK_Fail and
//      the file is never opened — OSF RE, 1.16.244. The shipped-event slot is the way in.)
//
// Positioning: v1 posts on the player's Wwise game object (the engine special-case ID 2),
// i.e. at the listener. SetPosition is bound, but a positioned post needs an OSF-owned game
// object whose REGISTRATION call is not yet bound for plugins (AddrLib 73402, an OSF RE
// follow-up). Scene cues fire near the player, so the v1 difference is attenuation, not
// correctness. Do NOT hand-synthesize the registration.
//
// Bus limitation: Dialogue_6_Combat is a dialogue/VO event, so external posts route through
// the dialogue bus (dialogue-volume gated, may duck other audio). Acceptable for v1; for
// cleaner SFX routing, request a content-neutral SFX event carrying the 0x24DB9834 cookie
// from OSF RE and swap the event ID in WwiseBackend.cpp — same code, cleaner bus.
//
// Safety: the PostEvent address is prologue-checked once before we trust the binding (the
// bytes match on 1.16.242 and 1.16.244); on a mismatch the whole Wwise path self-disables.
//
// Threading: PostEvent only enqueues into AK's command queue and is safe to call from any
// thread, so firing cues from the animation job threads needs no marshaling. The engine
// deep-copies the AkExternalSourceInfo (and its file-path string) into the queued command,
// so a caller-local path string is safe.

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
	// Gates EVERY Wwise call below.
	bool Available();

	// Fire-and-forget BAKED-event post on the player's game object (no external source).
	// Returns the AkPlayingID (0 = the engine rejected it — usually an event ID that is
	// not in any loaded bank). Safe from any thread; no-op when !Available().
	std::uint32_t PostEvent(std::uint32_t a_eventID);

	// --- external-source (loose-file) path ---

	// Maps a file extension to the external-source codec we post it with. nullopt = a format
	// the engine's external source can't stream directly (needs offline conversion or a PCM
	// decode first). .wav->kPCM, .wem->kVorbis, .xwm->kXWMA.
	std::optional<RE::BGSAudio::AkCodecID> CodecForExtension(std::string_view a_path);

	// Posts a loose file as an external source through a shipped event's "External_Source" slot,
	// on the player's game object (v1, at-listener). No bank load is needed — the event is
	// already resident. a_path is the game's Data-RELATIVE convention ('Data\...', wide), NOT
	// absolute — an absolute path is accepted but never opened (silent), RE-proven on 1.16.244.
	// Returns the AkPlayingID (0 = rejected / !Available()). Safe from any thread.
	std::uint32_t PostExternalFile(const std::wstring& a_path, RE::BGSAudio::AkCodecID a_codec);

	// Milestone-0 self-test: posts a_wav (a PCM .wav, Data-relative path) as an external source
	// with idCodec=kPCM on the shipped event Dialogue_6_Combat and logs the playingID. Lets the
	// PCM-direct mechanism be confirmed at boot before any scene runs. Nonzero playingID = accepted;
	// AUDIBLE must still be confirmed by ear (a queued event resolves the file async, and a dialogue
	// event may be quiet at the main menu). Logs and returns when !Available().
	void RunSelfTest(const std::wstring& a_wav);
}
