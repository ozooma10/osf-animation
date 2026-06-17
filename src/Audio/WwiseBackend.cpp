#include "Audio/WwiseBackend.h"

#include <array>
#include <cctype>
#include <charconv>
#include <cstring>

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

		// Lower-level external post on a chosen event/object. Builds the descriptor exactly as
		// the game does (SDK-confirmed 0x20 layout, default "External_Source" cookie). The
		// engine deep-copies the descriptor + path string into the queued command, so a_path
		// only needs to outlive this call. a_path is the game's Data-RELATIVE convention
		// ('Data\...'), NOT absolute — an absolute path is accepted but never opened (silent).
		std::uint32_t PostExternalOn(std::uint32_t a_eventID, const std::wstring& a_path,
			RE::BGSAudio::AkCodecID a_codec, std::uint64_t a_gameObj)
		{
			RE::BGSAudio::AkExternalSourceInfo ext{};
			ext.iExternalSrcCookie = RE::BGSAudio::kExternalSourceCookie;
			ext.idCodec = static_cast<std::uint32_t>(a_codec);
			ext.szFile = const_cast<wchar_t*>(a_path.c_str());
			ext.pInMemory = nullptr;
			ext.uiMemorySize = 0;
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

	std::optional<RE::BGSAudio::AkCodecID> CodecForExtension(std::string_view a_path)
	{
		const auto ext = LowerExtension(a_path);
		if (ext == "wav") {
			return RE::BGSAudio::AkCodecID::kPCM;  // Milestone-0 hypothesis: loose PCM .wav plays direct
		}
		if (ext == "wem") {
			return RE::BGSAudio::AkCodecID::kVorbis;  // engine default external codec
		}
		if (ext == "xwm") {
			return RE::BGSAudio::AkCodecID::kXWMA;
		}
		return std::nullopt;  // mp3/ogg/etc. — needs a PCM decode first (deferred)
	}

	std::uint32_t PostExternalFile(const std::wstring& a_path, RE::BGSAudio::AkCodecID a_codec)
	{
		if (!Available()) {
			return 0;
		}
		return PostExternalOn(kExternalSourceEvent, a_path, a_codec,
			RE::BGSAudio::AkSoundEngine::kPlayerGameObject);
	}

	void RunSelfTest(const std::wstring& a_wav)
	{
		if (!Available()) {
			REX::WARN("WwiseBackend self-test: AK PostEvent unavailable on this runtime — skipped");
			return;
		}
		REX::INFO("WwiseBackend self-test (Milestone 0): posting a PCM .wav as an external source on "
		          "the shipped event Dialogue_6_Combat. playingID != 0 = the engine ACCEPTED it; "
		          "AUDIBLE must still be confirmed by ear (file resolves async, and a dialogue event "
		          "may be quiet at the main menu). File: '{}'",
			std::filesystem::path(a_wav).string());

		const auto playingID = PostExternalOn(kExternalSourceEvent, a_wav,
			RE::BGSAudio::AkCodecID::kPCM, RE::BGSAudio::AkSoundEngine::kPlayerGameObject);
		REX::INFO("WwiseBackend self-test: event Dialogue_6_Combat (0x{:08X}) -> playingID {} ({})",
			kExternalSourceEvent, playingID, playingID ? "ACCEPTED" : "REJECTED");
	}
}
