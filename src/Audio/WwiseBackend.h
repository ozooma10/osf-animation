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

	// --- external-source (.wem) path ---

	// True only for a Wwise-encoded .wem, the only format the engine-mixed Wwise external-source path seems to accept 
	// (AK rejects a vanilla .wav: accepted but SILENT). Returns false for .wav/.mp3/etc, which SoundService::Play routes to the miniaudio device (NOT engine-mixed);
	// convert audio to .wem (e.g. WwiseConsole) to get it engine-mixed.
	bool IsWwiseExternalSource(std::string_view a_path);

	// Plays a .wem as an external source through a shipped event's "External_Source" slot, on the player's game object (at-listener). 
	// No bank load is needed — the event is already resident. 
	// a_path is opened through the process file API (game-root-relative 'Data\...', so MO2/USVFS loose files resolve), loaded ONCE into a process-lifetime media cache, 
	// posted via pInMemory; the post codec is read from the .wem's fmt tag (Vorbis or PCM). The caller manages no memory and may free a_path immediately. 
	// Returns the AkPlayingID (0 = rejected / load failed / !Available()). Safe from any thread. Caller must gate on IsWwiseExternalSource first.
	std::uint32_t PostExternalFile(const std::wstring& a_path);


	//==== DEBUG TEST =====
	// Self-test: posts a_wem (a Wwise .wem, Data-relative path) as an external source on the shipped event Dialogue_6_Combat and logs the playingID. 
	// Lets the external-source path be confirmed at boot before any scene runs. Nonzero playingID = accepted; 
	void RunSelfTest(const std::wstring& a_wem);
}
