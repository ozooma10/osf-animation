#include "Audio/SoundService.h"

#include "Audio/WwiseBackend.h"

#include <algorithm>
#include <chrono>

// Header-only package: exactly this TU compiles the implementation. 
// The implementation is C and not /Wall-clean, drop it from our warning level.
#pragma warning(push, 0)
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include <miniaudio.h>
#pragma warning(pop)

namespace OSF::Audio
{
	namespace
	{
		// One process-lifetime engine: device + resource manager + spatializer.
		// Never uninitialized, the process teardown reclaims it, and tearing down an audio device from DllMain-adjacent paths is a hang.
		ma_engine g_engine;

		// Attenuation tuned for room-scale voice: full volume inside ~1.5 m, inverse falloff to inaudible past ~30 m (world units are meters).
		constexpr float kMinDistance = 1.5f;
		constexpr float kMaxDistance = 30.0f;
	}

	struct SoundService::ActiveSound
	{
		ma_sound sound{};
		// Async loads that fail never reach "at end", reap by age as backstop.
		std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
		// Voice-channel key this sound occupies (0 = unslotted). On reap we clear the matching slot so
		// a later replace never dereferences this freed sound.
		std::uint64_t slot = 0;
	};

	SoundService& SoundService::GetSingleton()
	{
		static SoundService instance;
		return instance;
	}

	void SoundService::Init()
	{
		std::scoped_lock l{ lock };
		if (initAttempted) {
			return;
		}
		initAttempted = true;

		// The engine-native path needs no setup here: loose files post as external sources through a SHIPPED event that already carries an "External_Source" slot (WwiseBackend),
		// so they ride the game's Wwise mix with no bank to load. The miniaudio device below is the legacy fallback, used only for codecs the external source can't stream directly.

		if (ma_engine_init(nullptr, &g_engine) != MA_SUCCESS) {
			REX::WARN("[Audio] audio device init failed, cue sounds disabled (cue events still dispatch)");
			return;
		}
		// Starfield is Z-up; miniaudio defaults to Y-up. Panning reads the listener frame, so fix the world up once here (position/direction follow the player every Tick).
		ma_engine_listener_set_world_up(&g_engine, 0, 0.0f, 0.0f, 1.0f);
		engineReady = true;
		REX::DEBUG("[Audio] audio engine ready ({} Hz, {} ch)", ma_engine_get_sample_rate(&g_engine), ma_engine_get_channels(&g_engine));
	}

	void SoundService::Play(std::uint64_t a_slot, const std::string& a_dataRelPath, const RE::NiPoint3& a_worldPos, float a_volume)
	{
		// "event:" specs post through the game's own Wwise engine as a baked event, engine-mixed (sliders/pause/ducking apply)
		// fired on the player's game object so position and a_volume do not apply (the mix is engine-owned). See WwiseBackend.h.
		if (const auto eventID = Wwise::ParseEventSpec(a_dataRelPath)) {
			const auto playingID = Wwise::PostEvent(*eventID);
			if (playingID == 0) {
				REX::WARN("[Audio] Wwise rejected '{}' (event 0x{:08X} not in any loaded bank?)", a_dataRelPath, *eventID);
			} else {
				// Replace any prior voice on this slot (cuts the prior Wwise voice once the AK stop is proven).
				std::scoped_lock l{ lock };
				ClearSlotLocked(a_slot);
				if (a_slot != 0) {
					slots[a_slot].wwisePlayingID = playingID;
				}
				REX::DEBUG("[Audio] posted Wwise event '{}' (0x{:08X}) -> playingID {} (slot {:#x})", a_dataRelPath, *eventID, playingID, a_slot);
			}
			return;
		}

		// Plain file path. A .wem (posted as-is) OR a .wav/.mp3/.ogg/.flac (decoded to PCM and wrapped in a PCM .wem at runtime by the backend) 
		// rides the engine-mixed Wwise EXTERNAL SOURCE path through a shipped event's "External_Source" slot, AT THE LISTENER;
		// a_worldPos / a_volume do NOT apply on this path (the mix is engine-owned; positioned posting is a deferred follow-up via SetPosition, AddrLib 150420). 
		// A codec miniaudio can't decode falls through to the device below.
		if (Wwise::Available() && Wwise::IsWwiseExternalSource(a_dataRelPath)) {
			// The backend opens this path itself (game-root-relative), prepares the bytes, and posts via pInMemory
			const auto rel = (std::filesystem::path("Data") / a_dataRelPath).make_preferred().wstring();
			if (const auto playingID = Wwise::PostExternalFile(rel)) {
				// Replace any prior voice on this slot (cuts the prior Wwise voice once the AK stop is proven).
				std::scoped_lock l{ lock };
				ClearSlotLocked(a_slot);
				if (a_slot != 0) {
					slots[a_slot].wwisePlayingID = playingID;
				}
				REX::DEBUG("[Audio] posted external '{}' -> playingID {} (engine-mixed, slot {:#x})", a_dataRelPath, playingID, a_slot);
				return;
			}
			REX::WARN("[Audio] Wwise external-source post rejected '{}' — falling back to the miniaudio device",
				a_dataRelPath);
		}

		// legacy miniaudio fallback (bypasses the game mix). 
		Init();  // no-op after the first attempt; normally kPostDataLoad paid this

		std::scoped_lock l{ lock };
		if (!engineReady) {
			return;
		}

		const auto path = (std::filesystem::current_path() / "Data" / a_dataRelPath).string();
		auto active = std::make_unique<ActiveSound>();

		// ASYNC+STREAM: file IO/decode happens on miniaudio's worker path. 
		// Do not use MA_SOUND_FLAG_DECODE here; that creates a decode-only sound which opens successfully but is not routed to the engine for playback.
		constexpr ma_uint32 flags = MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_ASYNC;
		if (ma_sound_init_from_file(&g_engine, path.c_str(), flags, nullptr, nullptr, &active->sound) != MA_SUCCESS) {
			REX::WARN("[Audio] cannot open '{}'", path);
			return;
		}

		ma_sound_set_positioning(&active->sound, ma_positioning_absolute);
		ma_sound_set_position(&active->sound, a_worldPos.x, a_worldPos.y, a_worldPos.z);
		ma_sound_set_attenuation_model(&active->sound, ma_attenuation_model_inverse);
		ma_sound_set_min_distance(&active->sound, kMinDistance);
		ma_sound_set_max_distance(&active->sound, kMaxDistance);
		ma_sound_set_doppler_factor(&active->sound, 0.0f);  // engine-paced clips, no pitch warble
		ma_sound_set_volume(&active->sound, std::clamp(a_volume, 0.0f, 2.0f));
		if (ma_sound_start(&active->sound) != MA_SUCCESS) {
			REX::WARN("[Audio] failed to start '{}'", path);
			ma_sound_uninit(&active->sound);
			return;
		}
		REX::DEBUG("[Audio] playing '{}' at ({:.2f},{:.2f},{:.2f}) volume {:.2f} (slot {:#x})", a_dataRelPath, a_worldPos.x, a_worldPos.y, a_worldPos.z, a_volume, a_slot);

		// Replace any prior sound on this slot (cuts the prior miniaudio sound AND any prior Wwise voice),
		// then take ownership of the slot. The new sound is already started, so the channel never goes silent.
		ClearSlotLocked(a_slot);
		if (a_slot != 0) {
			active->slot = a_slot;
			slots[a_slot].miniSound = active.get();
		}

		sounds.push_back(std::move(active));
		activeCount.store(static_cast<int>(sounds.size()), std::memory_order_relaxed);
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
		const SlotOwner owner = it->second;
		slots.erase(it);

		// Cut the prior Wwise voice (no-op until the AK stop entry is runtime-proven; harmless on a
		// playingID the engine has already retired).
		if (owner.wwisePlayingID != 0) {
			Wwise::StopVoice(owner.wwisePlayingID);
		}
		// Cut the prior miniaudio sound and drop it from the live set.
		if (owner.miniSound != nullptr) {
			std::erase_if(sounds, [&](const std::unique_ptr<ActiveSound>& s) {
				if (s.get() == owner.miniSound) {
					ma_sound_uninit(&s->sound);
					return true;
				}
				return false;
			});
			activeCount.store(static_cast<int>(sounds.size()), std::memory_order_relaxed);
		}
	}

	void SoundService::Tick()
	{
		if (activeCount.load(std::memory_order_relaxed) == 0) {
			return;
		}

		std::scoped_lock l{ lock };
		if (!engineReady || sounds.empty()) {
			return;
		}

		// Listener follows the player; heading drives the pan frame. data fields are plain reads off another thread's state
		// tolerable for audio, same tolerance the adjust-hotkeys path already accepts.
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			const auto& pos = player->data.location;
			const float heading = player->data.angle.z;  // radians, Z-up yaw
			ma_engine_listener_set_position(&g_engine, 0, pos.x, pos.y, pos.z);
			ma_engine_listener_set_direction(&g_engine, 0, std::sin(heading), std::cos(heading), 0.0f);
		}

		ReapLocked();
	}

	void SoundService::ReapLocked()
	{
		const auto now = std::chrono::steady_clock::now();
		std::erase_if(sounds, [&](const std::unique_ptr<ActiveSound>& s) {
			const bool oldEnoughForEnd = now - s->created > std::chrono::milliseconds(250);
			const bool done = (oldEnoughForEnd && ma_sound_at_end(&s->sound)) ||
			                  (!ma_sound_is_playing(&s->sound) && now - s->created > std::chrono::seconds(60));
			if (done) {
				// Release the voice slot this sound held so a later replace never touches the freed sound.
				if (s->slot != 0) {
					if (const auto it = slots.find(s->slot); it != slots.end() && it->second.miniSound == s.get()) {
						slots.erase(it);
					}
				}
				ma_sound_uninit(&s->sound);
			}
			return done;
		});
		activeCount.store(static_cast<int>(sounds.size()), std::memory_order_relaxed);
	}

	void SoundService::SetVolume(float a_volume)
	{
		std::scoped_lock l{ lock };
		if (engineReady) {
			ma_engine_set_volume(&g_engine, std::clamp(a_volume, 0.0f, 2.0f));
		}
	}

	void SoundService::StopAll()
	{
		std::scoped_lock l{ lock };
		// Cut any tracked Wwise voices too (no-op until the AK stop entry is proven), then drop all slots.
		for (const auto& [key, owner] : slots) {
			if (owner.wwisePlayingID != 0) {
				Wwise::StopVoice(owner.wwisePlayingID);
			}
		}
		slots.clear();
		for (auto& s : sounds) {
			ma_sound_uninit(&s->sound);
		}
		sounds.clear();
		activeCount.store(0, std::memory_order_relaxed);
	}
}
