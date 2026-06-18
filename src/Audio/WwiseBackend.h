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
//   2. EXTERNAL SOURCE (.wem modder files) — any event that carries an "External_Source"
//      placeholder slot lets PostEvent substitute caller-supplied media via the trailing
//      (cExternals, AkExternalSourceInfo*) args. We post through the slot of a SHIPPED event
//      already resident in a loaded bank ("Dialogue_6_Combat", AkUniqueID 0x5DE4F1F3) — so a
//      loose .wem plays engine-mixed with NO soundbank authoring and NO LoadBank. The media is
//      provided via pInMemory (BYTES), NOT szFile (a path): the engine's audio file resolver is
//      BSResource-registry-gated and never opens a loose file, so a szFile post is accepted
//      (nonzero playingID) but SILENT — proven 1.16.244 (OSF RE .../2026-06-17-wwise-plugin-
//      external-post-silent.md; mechanism in module engine.resource_loading). The .wem bytes live
//      in a process-lifetime cache INSIDE this backend, so CALLERS NEVER MANAGE MEMORY — just pass
//      a path. The media MUST be a Wwise-encoded .wem (Vorbis OR PCM, read from its fmt tag); a
//      vanilla .wav/.mp3 is NOT engine-mixable and is routed to the miniaudio device by
//      SoundService instead (see IsWwiseExternalSource). (A custom mod .bnk is likewise dead:
//      LoadBank is registry-gated, returns AK_Fail.)
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
// thread, so firing cues from the animation job threads needs no marshaling. AK copies the
// AkExternalSourceInfo descriptor into the queued command, but does NOT copy the pInMemory
// bytes — it references them across the whole playback. The backend's media cache owns those
// bytes for the process lifetime, so they always outlive the voice (callers manage nothing).

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

	// --- external-source (.wem) path ---

	// True only for a Wwise-encoded .wem — the one format the engine-mixed Wwise external-source
	// path accepts (AK rejects a vanilla .wav: accepted but SILENT, proven 1.16.244). Returns false
	// for .wav/.mp3/etc, which SoundService::Play routes to the miniaudio device (NOT engine-mixed);
	// convert audio to .wem (e.g. WwiseConsole) to get it engine-mixed.
	bool IsWwiseExternalSource(std::string_view a_path);

	// Plays a .wem as an external source through a shipped event's "External_Source" slot, on the
	// player's game object (at-listener). No bank load is needed — the event is already resident.
	// a_path is opened through the process file API (game-root-relative 'Data\...', so MO2/USVFS
	// loose files resolve), loaded ONCE into a process-lifetime media cache, and posted via
	// pInMemory; the post codec is read from the .wem's fmt tag (Vorbis or PCM). The caller manages
	// no memory and may free a_path immediately. Returns the AkPlayingID (0 = rejected / load failed
	// / !Available()). Safe from any thread. Caller must gate on IsWwiseExternalSource first.
	std::uint32_t PostExternalFile(const std::wstring& a_path);

	// Self-test: posts a_wem (a Wwise .wem, Data-relative path) as an external source on the shipped
	// event Dialogue_6_Combat and logs the playingID. Lets the external-source path be confirmed at
	// boot before any scene runs. Nonzero playingID = accepted; AUDIBLE must still be confirmed by
	// ear (a dialogue event may be quiet at the main menu). Logs and returns when !Available().
	void RunSelfTest(const std::wstring& a_wem);
}
