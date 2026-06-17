#include "Audio/WwiseBackend.h"

#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdlib>
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

		// The mod-shipped placeholder bank + its single external-source event.
		// LoadBank appends ".bnk" and resolves via the engine IO hook from
		// Data\Sound\Soundbanks\.
		constexpr const char* kPlaceholderBank = "OSF_Placeholder";
		constexpr const char* kPlaceholderEvent = "OSF_PlayExternal";

		// Proven shipped event that already carries an "External_Source" slot and played a
		// loose file live (RE capture, 1.16.244). Used ONLY by RunSelfTest so the external
		// mechanism can be confirmed before the placeholder bank is authored — NOT a
		// production fallback (a combat-dialogue event is RTPC/ducking-gated for real cues).
		constexpr std::uint32_t kFallbackEventDialogue6Combat = 0x5DE4F1F3;

		constexpr std::string_view kEventPrefix = "event:";

		// Set once at startup (kPostDataLoad, single-threaded), read from job threads after.
		std::atomic<bool>          g_bankLoaded{ false };
		std::atomic<std::uint32_t> g_placeholderEventID{ 0 };

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
		// engine deep-copies the descriptor + path string into the queued command, so a_absPath
		// only needs to outlive this call.
		std::uint32_t PostExternalOn(std::uint32_t a_eventID, const std::wstring& a_absPath,
			RE::BGSAudio::AkCodecID a_codec, std::uint64_t a_gameObj)
		{
			RE::BGSAudio::AkExternalSourceInfo ext{};
			ext.iExternalSrcCookie = RE::BGSAudio::kExternalSourceCookie;
			ext.idCodec = static_cast<std::uint32_t>(a_codec);
			ext.szFile = const_cast<wchar_t*>(a_absPath.c_str());
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

	bool LoadPlaceholderBank()
	{
		if (!Available()) {
			return false;
		}
		if (g_bankLoaded.load(std::memory_order_acquire)) {
			return true;  // idempotent
		}

		RE::BGSAudio::AkSoundEngine::AkBankID bankID = 0;
		const auto result = RE::BGSAudio::AkSoundEngine::LoadBank(kPlaceholderBank, bankID);
		if (result != RE::BGSAudio::AkSoundEngine::kAkSuccess) {
			REX::WARN("WwiseBackend: LoadBank(\"{}\") returned {} (expected kAkSuccess {}) — placeholder "
			          "external-source path OFF; ship Data\\Sound\\Soundbanks\\{}.bnk. Loose-file cues "
			          "use the legacy device until then.",
				kPlaceholderBank, result, RE::BGSAudio::AkSoundEngine::kAkSuccess, kPlaceholderBank);
			return false;
		}

		const auto eventID = RE::BGSAudio::AkSoundEngine::GetIDFromString(kPlaceholderEvent);
		g_placeholderEventID.store(eventID, std::memory_order_relaxed);
		g_bankLoaded.store(true, std::memory_order_release);
		REX::INFO("WwiseBackend: placeholder bank \"{}\" loaded (bankID {}), event \"{}\" -> 0x{:08X} — "
		          "loose files now play through the game's Wwise mix",
			kPlaceholderBank, bankID, kPlaceholderEvent, eventID);

		// Symmetric teardown the brief asks for. atexit runs during normal CRT shutdown, before
		// the statically-linked Wwise destructors; a single bank unload is cheap and avoids the
		// DllMain-teardown hazard the legacy device deliberately skips.
		std::atexit([]() { UnloadPlaceholderBank(); });
		return true;
	}

	void UnloadPlaceholderBank()
	{
		if (!Available() || !g_bankLoaded.exchange(false, std::memory_order_acq_rel)) {
			return;
		}
		// File-loaded bank -> nullptr memPtr.
		const auto result = RE::BGSAudio::AkSoundEngine::UnloadBank(kPlaceholderBank, nullptr);
		REX::INFO("WwiseBackend: UnloadBank(\"{}\") returned {}", kPlaceholderBank, result);
	}

	bool ExternalReady()
	{
		return g_bankLoaded.load(std::memory_order_acquire);
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

	std::uint32_t PostExternalFile(const std::wstring& a_absPath, RE::BGSAudio::AkCodecID a_codec)
	{
		if (!Available() || !ExternalReady()) {
			return 0;
		}
		return PostExternalOn(g_placeholderEventID.load(std::memory_order_relaxed), a_absPath, a_codec,
			RE::BGSAudio::AkSoundEngine::kPlayerGameObject);
	}

	void RunSelfTest(const std::wstring& a_absWav)
	{
		if (!Available()) {
			REX::WARN("WwiseBackend self-test: AK PostEvent unavailable on this runtime — skipped");
			return;
		}
		REX::INFO("WwiseBackend self-test (Milestone 0): posting a PCM .wav as an external source. "
		          "playingID != 0 = the engine ACCEPTED it; AUDIBLE must still be confirmed by ear "
		          "(file resolves async). File: '{}'",
			std::filesystem::path(a_absWav).string());

		const auto codec = RE::BGSAudio::AkCodecID::kPCM;
		const auto obj = RE::BGSAudio::AkSoundEngine::kPlayerGameObject;

		if (ExternalReady()) {
			const auto id = g_placeholderEventID.load(std::memory_order_relaxed);
			const auto playingID = PostExternalOn(id, a_absWav, codec, obj);
			REX::INFO("WwiseBackend self-test: placeholder event \"{}\" (0x{:08X}) -> playingID {} ({})",
				kPlaceholderEvent, id, playingID, playingID ? "ACCEPTED" : "REJECTED");
		} else {
			REX::INFO("WwiseBackend self-test: placeholder bank not loaded — skipping its post "
			          "(ship Data\\Sound\\Soundbanks\\{}.bnk to test the production event)",
				kPlaceholderBank);
		}

		// Always also try the proven shipped event so the mechanism can be confirmed even with
		// no authored bank. May be quiet at the main menu if dialogue isn't active — the
		// placeholder event is the authoritative test.
		const auto fbID = PostExternalOn(kFallbackEventDialogue6Combat, a_absWav, codec, obj);
		REX::INFO("WwiseBackend self-test: fallback event Dialogue_6_Combat (0x{:08X}) -> playingID {} ({})",
			kFallbackEventDialogue6Combat, fbID, fbID ? "ACCEPTED" : "REJECTED");
	}
}
