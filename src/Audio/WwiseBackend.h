#pragma once

// Posts through the game's statically linked Wwise (RE::BGSAudio::AkSoundEngine), so the game mix applies.

namespace OSF::Audio::Wwise
{
	// AK::SoundEngine::GetIDFromString: FNV-1 32-bit over the lowercased name (basis 0x811C9DC5, prime 0x1000193).
	std::uint32_t HashEventName(std::string_view a_name);

	// "event:" spec -> AkUniqueID (hex literal verbatim, else hashed); nullopt for file paths / "$pool" refs.
	std::optional<std::uint32_t> ParseEventSpec(std::string_view a_spec);

	// gameObject 0 = listener. Returns the AkPlayingID (0 = rejected, usually an event not in a loaded bank). Any thread.
	std::uint32_t PostEvent(std::uint32_t a_eventID, std::uint64_t a_gameObject = 0);

	// Any thread; harmless on a retired playing ID. True when a stop was issued.
	bool StopVoice(std::uint32_t a_playingID);

	// .wem or a miniaudio-decodable format. A raw .wav posted as bytes is silent, hence the decode.
	bool IsWwiseExternalSource(std::string_view a_path);

	// Game-root-relative 'Data\...' path via the process file API (so MO2/USVFS resolves), prepared once into a process-lifetime cache.
	// Returns the AkPlayingID (0 = failed). Any thread. Gate on IsWwiseExternalSource first.
	std::uint32_t PostExternalFile(const std::wstring& a_path, std::uint64_t a_gameObject = 0);
}
