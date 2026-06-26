#pragma once

// Engine-native sound playback through the game's own statically linked Wwise engine (RE::BGSAudio::AkSoundEngine, the CommonLibSF fork bindings). 
// Everything posted here rides the game mix, master/SFX volume sliders, the pause menu, ducking and busses all apply

namespace OSF::Audio::Wwise
{
	// reimplementation of AK::SoundEngine::GetIDFromString: FNV-1 32-bit over the lowercased name (basis 0x811C9DC5, prime 0x1000193), 
	// matching the engine's own hasher exactly. Lets packs name events directly, with no soundbank metadata lookup.
	std::uint32_t HashEventName(std::string_view a_name);

	// Recognizes the "event:" cue-sound spec. 
	// Returns the AkUniqueID (hex literal taken verbatim, anything else hashed), or nullopt when a_spec is not an event reference (i.e. it is a file path / "$pool" ref).
	std::optional<std::uint32_t> ParseEventSpec(std::string_view a_spec);

	bool Available();

	// Fire-and-forget BAKED-event post on the player's game object (no external source).
	// Returns the AkPlayingID (0 = the engine rejected it, usually an event ID that is not in any loaded bank).
	// Safe from any thread; no-op when !Available().
	std::uint32_t PostEvent(std::uint32_t a_eventID);

	// Stops a single posted voice by its AkPlayingID (the value PostEvent / PostExternalFile returned),
	// for per-slot voice replacement. Safe from any thread; harmless on a playingID the engine already retired.
	//
	// PROOF-GATED: the AK stop entry point (AK::SoundEngine::StopPlayingID / ExecuteActionOnPlayingID) is not
	// yet RE-proven on this build, so calling an unverified AK function with a playingID could corrupt the
	// audio command queue. Until the rel_id is identified and proven in-game (see WwiseBackend.cpp
	// kAkStopVoiceID + the OSF RE `wwise stopid` probe), this is a SAFE NO-OP. Returns true only if it
	// actually issued a stop, so callers/telemetry can tell whether Wwise replace is live yet.
	bool StopVoice(std::uint32_t a_playingID);

	// --- external-source (.wem) path ---

	// True for formats that ride the engine-mixed Wwise external-source path: a real .wem (posted as bytes) or a .wav/.mp3/.ogg/.flac (decoded to PCM and wrapped in a PCM .wem at runtime). 
	// A RAW vanilla .wav posted as bytes is silent, which is why non-.wem inputs are decoded first. Returns false for anything miniaudio can't decode;
	bool IsWwiseExternalSource(std::string_view a_path);

	// Plays a loose audio file as an external source through a shipped event's "External_Source" slot, on the player's game object (AT-LISTENER; position / per-cue volume do not apply on this path).
	// a_path is opened through the process file API (game-root-relative 'Data\...', so MO2/USVFS loose files resolve) and prepared ONCE into a process-lifetime media cache: 
	// a .wem is cached as-is; a .wav/.mp3/.ogg/.flac is decoded to PCM and wrapped in a PCM .wem. Posted via pInMemory (codec from the .wem fmt tag, or PCM for a built one).
	// The caller manages no memory and may free a_path immediately. Returns the AkPlayingID (0 = rejected / load failed / !Available()). Safe from any thread. Gate on IsWwiseExternalSource first.
	std::uint32_t PostExternalFile(const std::wstring& a_path);


	//==== DEBUG TEST =====
	// Self-test: posts a_file (a Data-relative .wem or decodable .wav/.mp3/.ogg/.flac) as an external source on the shipped event Dialogue_6_Combat and logs the playingID. 
	// Lets the external-source path be confirmed at boot before any scene runs. Nonzero playingID = accepted (AUDIBLE by ear).
	void RunSelfTest(const std::wstring& a_file);
}
