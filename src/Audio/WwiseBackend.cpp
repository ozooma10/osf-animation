#include "Audio/WwiseBackend.h"

#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <malloc.h>
#include <mutex>
#include <unordered_map>

namespace OSF::Audio::Wwise
{
	namespace
	{
		// AK::SoundEngine::PostEvent(AkUniqueID, AkGameObjectID, u32 flags,
		// callback, cookie, u32 cExternals, AkExternalSourceInfo*, AkPlayingID)
		// — the Wwise 2021.1 ABI, statically linked into the game. Confirmed
		// callable on 1.16.244: called from a worker thread, the returned playing
		// IDs continued the engine's own counter. We call it through the CommonLibSF
		// fork binding (RE::BGSAudio::AkSoundEngine); this ID is only the gate target.
		constexpr REL::ID kAkPostEventByID{ 150391 };

		// First 16 bytes of 150391, byte-identical on 1.16.242 and 1.16.244
		// (verified by the RE probe). Mismatch on a future patch = self-disable.
		constexpr std::array<std::uint8_t, 16> kPostEventPrologue{
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
			0x48, 0x89, 0x74, 0x24, 0x18, 0x4C
		};

		// Shipped event that already carries an "External_Source" placeholder slot and streamed a
		// loose file live (RE capture, 1.16.244 — OSF RE Investigations/Responses/
		// 2026-06-17-wwise-external-loose-audio.md). It is already resident in a loaded bank, so
		// NO LoadBank is needed: we just substitute our own media through its external-source slot.
		// (Equivalent at runtime: AkSoundEngine::GetIDFromString("Dialogue_6_Combat").)
		//
		// LIMITATION: this is a dialogue/VO event, so posts route through the dialogue bus
		// (dialogue-volume gated, may duck other audio). Fine for v1; for cleaner SFX routing,
		// swap kExternalSourceEvent to a content-neutral SFX event that carries the 0x24DB9834
		// cookie (an OSF RE HIRC scan of the loaded banks) — same code, cleaner bus.
		constexpr std::uint32_t kExternalSourceEvent = 0x5DE4F1F3;  // Ak hash of "Dialogue_6_Combat"

		constexpr std::string_view kEventPrefix = "event:";

		// Lowercased file extension (without the dot), or "" if none.
		std::string LowerExtension(std::string_view a_path)
		{
			const auto dot = a_path.find_last_of('.');
			if (dot == std::string_view::npos) {
				return {};
			}
			std::string ext{ a_path.substr(dot + 1) };
			for (auto& c : ext) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return ext;
		}

		// A Wwise .wem held in memory. AK references pInMemory zero-copy and reads it across the
		// whole playback, so the cache OWNS this memory for the process lifetime (never freed).
		// `codec` is the AkCodecID to post with — read from the .wem's own fmt tag, since a .wem may
		// be Vorbis OR PCM (WwiseConsole's default external-source conversion, for instance, is PCM).
		struct MediaBuffer
		{
			void* data{ nullptr };
			std::uint32_t size{ 0 };
			RE::BGSAudio::AkCodecID codec{ RE::BGSAudio::AkCodecID::kVorbis };
		};

		constexpr std::size_t kMaxMediaBytes = 64u << 20;  // 64 MiB guard per clip

		std::mutex g_mediaCacheMutex;
		std::unordered_map<std::wstring, MediaBuffer> g_mediaCache;

		// AkCodecID from a .wem's RIFF 'fmt ' tag: 0xFFFF = Wwise Vorbis, 0xFFFE = Wwise PCM.
		// Anything unrecognized -> Vorbis (the most common case); a non-.wem file won't match here.
		RE::BGSAudio::AkCodecID DeriveWemCodec(const void* a_data, std::uint32_t a_size)
		{
			const auto* b = static_cast<const std::uint8_t*>(a_data);
			if (a_size < 12 || std::memcmp(b, "RIFF", 4) != 0 || std::memcmp(b + 8, "WAVE", 4) != 0) {
				return RE::BGSAudio::AkCodecID::kVorbis;
			}
			for (std::uint32_t i = 12; i + 8 <= a_size;) {
				std::uint32_t chunkSize = 0;
				std::memcpy(&chunkSize, b + i + 4, 4);
				if (std::memcmp(b + i, "fmt ", 4) == 0 && i + 10 <= a_size) {
					std::uint16_t tag = 0;
					std::memcpy(&tag, b + i + 8, 2);
					return (tag == 0xFFFE) ? RE::BGSAudio::AkCodecID::kPCM
					                       : RE::BGSAudio::AkCodecID::kVorbis;
				}
				i += 8 + chunkSize + (chunkSize & 1);
			}
			return RE::BGSAudio::AkCodecID::kVorbis;
		}

		// Loads a Wwise .wem into a 16-byte-aligned, process-lifetime buffer (AK requires aligned
		// in-memory media and references it zero-copy). Opened through the process file API so
		// MO2/USVFS-virtual loose files resolve — unlike szFile, which the registry-gated audio
		// resolver never opens (OSF RE module engine.resource_loading), the reason we post bytes.
		MediaBuffer LoadWemFile(const std::wstring& a_path)
		{
			std::ifstream file{ a_path, std::ios::binary | std::ios::ate };
			if (!file) {
				REX::WARN("WwiseBackend: cannot open .wem '{}'", std::filesystem::path(a_path).string());
				return {};
			}
			const auto size = static_cast<std::size_t>(file.tellg());
			if (size == 0 || size > kMaxMediaBytes) {
				REX::WARN("WwiseBackend: .wem '{}' unusable size {}", std::filesystem::path(a_path).string(), size);
				return {};
			}
			void* buffer = _aligned_malloc(size, 16);
			if (buffer == nullptr) {
				return {};
			}
			file.seekg(0);
			if (!file.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size))) {
				_aligned_free(buffer);
				REX::WARN("WwiseBackend: .wem read failed '{}'", std::filesystem::path(a_path).string());
				return {};
			}
			const auto bytes = static_cast<std::uint32_t>(size);
			return MediaBuffer{ buffer, bytes, DeriveWemCodec(buffer, bytes) };
		}

		// Loads a .wem once and caches it for the process (success AND failure, so a missing file
		// neither re-hits disk nor spams the log on every cue).
		MediaBuffer GetOrLoadMedia(const std::wstring& a_path)
		{
			const std::scoped_lock lock{ g_mediaCacheMutex };
			if (const auto it = g_mediaCache.find(a_path); it != g_mediaCache.end()) {
				return it->second;
			}
			const MediaBuffer media = LoadWemFile(a_path);
			g_mediaCache.emplace(a_path, media);
			return media;
		}

		// Posts a .wem as an external source via pInMemory — NOT szFile: the engine's audio file
		// resolver is registry-gated and never opens a loose file, so a szFile post is accepted but
		// SILENT (proven 1.16.244). pInMemory bypasses the resolver and renders engine-mixed. The
		// cache owns the bytes for the process, so the descriptor only needs to outlive this call.
		std::uint32_t PostExternalOn(std::uint32_t a_eventID, const std::wstring& a_path, std::uint64_t a_gameObj)
		{
			const MediaBuffer media = GetOrLoadMedia(a_path);
			if (media.data == nullptr) {
				return 0;  // load failed (logged once by LoadWemFile)
			}
			RE::BGSAudio::AkExternalSourceInfo ext{};
			ext.iExternalSrcCookie = RE::BGSAudio::kExternalSourceCookie;
			ext.idCodec = static_cast<std::uint32_t>(media.codec);  // from the .wem's fmt tag
			ext.szFile = nullptr;
			ext.pInMemory = media.data;
			ext.uiMemorySize = media.size;
			ext.idFile = 0;
			return RE::BGSAudio::AkSoundEngine::PostEvent(
				a_eventID, a_gameObj, 0, nullptr, nullptr, 1, &ext, 0);
		}
	}

	std::uint32_t HashEventName(std::string_view a_name)
	{
		std::uint32_t hash = 0x811C9DC5;
		for (const char c : a_name) {
			hash *= 0x01000193;
			hash ^= static_cast<std::uint8_t>(std::tolower(static_cast<unsigned char>(c)));
		}
		return hash;
	}

	std::optional<std::uint32_t> ParseEventSpec(std::string_view a_spec)
	{
		if (!a_spec.starts_with(kEventPrefix)) {
			return std::nullopt;
		}
		auto body = a_spec.substr(kEventPrefix.size());
		if (body.empty()) {
			REX::WARN("WwiseBackend: empty 'event:' cue sound spec");
			return std::nullopt;
		}
		if (body.starts_with("0x") || body.starts_with("0X")) {
			body.remove_prefix(2);
			std::uint32_t id = 0;
			const auto [ptr, ec] = std::from_chars(body.data(), body.data() + body.size(), id, 16);
			if (ec != std::errc{} || ptr != body.data() + body.size()) {
				REX::WARN("WwiseBackend: malformed event ID in cue sound spec 'event:0x{}'", body);
				return std::nullopt;
			}
			return id;
		}
		return HashEventName(body);
	}

	bool Available()
	{
		static const bool available = []() {
			const auto* code = reinterpret_cast<const std::uint8_t*>(kAkPostEventByID.address());
			if (!code || std::memcmp(code, kPostEventPrologue.data(), kPostEventPrologue.size()) != 0) {
				REX::WARN("Wwise audio disabled: AK PostEvent (ID {}) prologue mismatch on this runtime "
				          "— loose-file cues fall back to the legacy device",
					kAkPostEventByID.id());
				return false;
			}
			REX::INFO("Wwise audio available: AK PostEvent prologue verified");
			return true;
		}();
		return available;
	}

	std::uint32_t PostEvent(std::uint32_t a_eventID)
	{
		if (!Available()) {
			return 0;
		}
		// cExternals 0 = no external source: a plain baked event on the player object.
		return RE::BGSAudio::AkSoundEngine::PostEvent(
			a_eventID, RE::BGSAudio::AkSoundEngine::kPlayerGameObject, 0, nullptr, nullptr, 0, nullptr, 0);
	}

	bool IsWwiseExternalSource(std::string_view a_path)
	{
		// Only a Wwise-encoded .wem rides the engine-mixed Wwise external-source path: AK external
		// sources reject a vanilla .wav (accepted but SILENT, proven 1.16.244). Every other format
		// returns false and SoundService::Play plays it on the miniaudio device (not engine-mixed);
		// convert audio to .wem (e.g. WwiseConsole) to get it engine-mixed.
		return LowerExtension(a_path) == "wem";
	}

	std::uint32_t PostExternalFile(const std::wstring& a_path)
	{
		if (!Available()) {
			return 0;
		}
		return PostExternalOn(kExternalSourceEvent, a_path, RE::BGSAudio::AkSoundEngine::kPlayerGameObject);
	}

	void RunSelfTest(const std::wstring& a_wem)
	{
		if (!Available()) {
			REX::WARN("WwiseBackend self-test: AK PostEvent unavailable on this runtime — skipped");
			return;
		}
		REX::INFO("WwiseBackend self-test: posting a .wem as an external source (pInMemory) on the shipped "
		          "event Dialogue_6_Combat. playingID != 0 = the engine ACCEPTED it; AUDIBLE must still be "
		          "confirmed by ear in-world (a dialogue event may be quiet at the main menu). File: '{}'",
			std::filesystem::path(a_wem).string());

		const auto playingID = PostExternalOn(kExternalSourceEvent, a_wem, RE::BGSAudio::AkSoundEngine::kPlayerGameObject);
		REX::INFO("WwiseBackend self-test: event Dialogue_6_Combat (0x{:08X}) -> playingID {} ({})",
			kExternalSourceEvent, playingID, playingID ? "ACCEPTED" : "REJECTED");
	}
}
