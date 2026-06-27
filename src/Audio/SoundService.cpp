#include "Audio/SoundService.h"

#include "Audio/WwiseBackend.h"

#include <filesystem>

namespace OSF::Audio
{
	SoundService& SoundService::GetSingleton()
	{
		static SoundService instance;
		return instance;
	}

	void SoundService::Play(std::uint64_t a_slot, const std::string& a_dataRelPath, [[maybe_unused]] const RE::NiPoint3& a_worldPos, float a_volume)
	{
		// "event:" specs post through the game's own Wwise engine as a baked event, engine-mixed (sliders/pause/ducking apply),
		// fired on the player's game object so a_worldPos and a_volume do not apply (the mix is engine-owned). See WwiseBackend.h.
		if (const auto eventID = Wwise::ParseEventSpec(a_dataRelPath)) {
			const auto playingID = Wwise::PostEvent(*eventID);
			if (playingID == 0) {
				REX::WARN("[Audio] Wwise rejected '{}' (event 0x{:08X} not in any loaded bank?) — cue skipped", a_dataRelPath, *eventID);
				return;
			}
			// Replace any prior voice on this slot — cuts the prior Wwise voice (Wwise::StopVoice is runtime-proven).
			std::scoped_lock l{ lock };
			ClearSlotLocked(a_slot);
			if (a_slot != 0) {
				slots[a_slot] = playingID;
			}
			REX::DEBUG("[Audio] posted Wwise event '{}' (0x{:08X}) -> playingID {} (slot {:#x})", a_dataRelPath, *eventID, playingID, a_slot);
			return;
		}

		// Plain file path. A .wem (posted as-is) OR a .wav/.mp3/.ogg/.flac (decoded to PCM and wrapped in a PCM .wem at runtime by the backend)
		// rides the engine-mixed Wwise EXTERNAL SOURCE path through a shipped event's "External_Source" slot, AT THE LISTENER;
		// a_worldPos / a_volume do NOT apply on this path (the mix is engine-owned; positioned posting is a deferred follow-up via SetPosition, AddrLib 150420).
		if (!Wwise::Available() || !Wwise::IsWwiseExternalSource(a_dataRelPath)) {
			REX::WARN("[Audio] no engine path for '{}' (Wwise {}; unsupported codec?) — cue skipped (vol {:.2f})",
				a_dataRelPath, Wwise::Available() ? "available" : "unavailable", a_volume);
			return;
		}

		// The backend opens this path itself (game-root-relative), prepares the bytes, and posts via pInMemory.
		const auto rel = (std::filesystem::path("Data") / a_dataRelPath).make_preferred().wstring();
		const auto playingID = Wwise::PostExternalFile(rel);
		if (playingID == 0) {
			REX::WARN("[Audio] Wwise external-source post rejected '{}' — cue skipped", a_dataRelPath);
			return;
		}
		// Replace any prior voice on this slot — cuts the prior Wwise voice (Wwise::StopVoice is runtime-proven).
		std::scoped_lock l{ lock };
		ClearSlotLocked(a_slot);
		if (a_slot != 0) {
			slots[a_slot] = playingID;
		}
		REX::DEBUG("[Audio] posted external '{}' -> playingID {} (engine-mixed, slot {:#x})", a_dataRelPath, playingID, a_slot);
	}

	void SoundService::ClearSlotLocked(std::uint64_t a_slot)
	{
		if (a_slot == 0) {
			return;
		}
		const auto it = slots.find(a_slot);
		if (it == slots.end()) {
			return;
		}
		const std::uint32_t playingID = it->second;
		slots.erase(it);

		// Cut the prior Wwise voice (Wwise::StopVoice is runtime-proven — AK ExecuteActionOnPlayingID 150360;
		// harmless on a playingID the engine has already retired).
		if (playingID != 0) {
			Wwise::StopVoice(playingID);
		}
	}

	void SoundService::StopAll()
	{
		std::scoped_lock l{ lock };
		// Cut every tracked Wwise voice (Wwise::StopVoice is runtime-proven), then drop all slots.
		for (const auto& [key, playingID] : slots) {
			if (playingID != 0) {
				Wwise::StopVoice(playingID);
			}
		}
		slots.clear();
	}
}
