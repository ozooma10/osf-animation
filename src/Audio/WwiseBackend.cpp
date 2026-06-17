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
		// IDs continued the engine's own counter.
		constexpr REL::ID kAkPostEventByID{ 150391 };

		// First 16 bytes of 150391, byte-identical on 1.16.242 and 1.16.244
		// (verified by the RE probe). Mismatch on a future patch = self-disable.
		constexpr std::array<std::uint8_t, 16> kPostEventPrologue{
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
			0x48, 0x89, 0x74, 0x24, 0x18, 0x4C
		};

		// Engine special case in WwiseGameObjectMgr::GetOrCreateGameObjectID:
		// the player TESObjectREFR always maps to game object 2 (live-confirmed
		// — all *_PC foley posts on it). Registered at startup, always valid.
		constexpr std::uint64_t kPlayerGameObject = 2;

		using AkPostEventByIDFn = std::uint32_t (*)(std::uint32_t a_eventID, std::uint64_t a_gameObj,
		                                            std::uint32_t a_flags, void* a_callback, void* a_cookie,
		                                            std::uint32_t a_cExternals, void* a_pExternals,
		                                            std::uint32_t a_playingID);

		constexpr std::string_view kEventPrefix = "event:";
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
				REX::WARN("Wwise event cues disabled: AK PostEvent (ID {}) prologue mismatch on this runtime "
				          "— file-path cue sounds are unaffected",
					kAkPostEventByID.id());
				return false;
			}
			REX::INFO("Wwise event cues available: AK PostEvent prologue verified");
			return true;
		}();
		return available;
	}

	std::uint32_t PostEvent(std::uint32_t a_eventID)
	{
		if (!Available()) {
			return 0;
		}
		const auto post = reinterpret_cast<AkPostEventByIDFn>(kAkPostEventByID.address());
		// flags 0 = no callback — fire-and-forget exactly as the probe proved.
		return post(a_eventID, kPlayerGameObject, 0, nullptr, nullptr, 0, nullptr, 0);
	}
}
