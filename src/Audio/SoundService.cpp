#include "Audio/SoundService.h"

#include "Audio/WwiseBackend.h"

#include <algorithm>
#include <chrono>

// Header-only package: exactly this TU compiles the implementation. The
// implementation is C and not /Wall-clean — drop it from our warning level.
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
		// Never uninitialized — the process teardown reclaims it, and tearing
		// down an audio device from DllMain-adjacent paths is a classic hang.
		ma_engine g_engine;

		// Attenuation tuned for room-scale voice: full volume inside ~1.5 m,
		// inverse falloff to inaudible past ~30 m (world units are meters).
		constexpr float kMinDistance = 1.5f;
		constexpr float kMaxDistance = 30.0f;
	}

	struct SoundService::ActiveSound
	{
		ma_sound sound{};
		// Async loads that fail never reach "at end" — reap by age as backstop.
		std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
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

		// The engine-native path needs no setup here: loose files post as external sources
		// through a SHIPPED event that already carries an "External_Source" slot (WwiseBackend),
		// so they ride the game's Wwise mix with no bank to load. The miniaudio device below is
		// the legacy fallback, used only for codecs the external source can't stream directly.

		if (ma_engine_init(nullptr, &g_engine) != MA_SUCCESS) {
			REX::WARN("SoundService: audio device init failed — cue sounds disabled (cue events still dispatch)");
			return;
		}
		// Starfield is Z-up; miniaudio defaults to Y-up. Panning reads the
		// listener frame, so fix the world up once here (position/direction
		// follow the player every Tick).
		ma_engine_listener_set_world_up(&g_engine, 0, 0.0f, 0.0f, 1.0f);
		engineReady = true;
		REX::INFO("SoundService: audio engine ready ({} Hz, {} ch)",
			ma_engine_get_sample_rate(&g_engine), ma_engine_get_channels(&g_engine));
	}

	void SoundService::Play(const std::string& a_dataRelPath, const RE::NiPoint3& a_worldPos, float a_volume)
	{
		if (!enabled.load(std::memory_order_relaxed)) {
			return;
		}

		// "event:" specs post through the game's own Wwise engine as a baked event —
		// engine-mixed (sliders/pause/ducking apply), fired on the player's game object
		// so position and a_volume do not apply (the mix is engine-owned). See WwiseBackend.h.
		if (const auto eventID = Wwise::ParseEventSpec(a_dataRelPath)) {
			const auto playingID = Wwise::PostEvent(*eventID);
			if (playingID == 0) {
				REX::WARN("SoundService: Wwise rejected '{}' (event 0x{:08X} not in any loaded bank?)",
					a_dataRelPath, *eventID);
			} else {
				REX::DEBUG("SoundService: posted Wwise event '{}' (0x{:08X}) -> playingID {}",
					a_dataRelPath, *eventID, playingID);
			}
			return;
		}

		// Plain file path. A Wwise-encoded .wem rides the engine-mixed Wwise EXTERNAL SOURCE path
		// (through a shipped event's "External_Source" slot, at the listener; a_worldPos / a_volume
		// don't apply — the mix is engine-owned). A raw .wav/.mp3 is NOT engine-mixable (AK external
		// sources reject a vanilla WAV — proven 1.16.244), so it falls through to the miniaudio
		// device below; convert audio to .wem to get it engine-mixed.
		if (Wwise::Available() && Wwise::IsWwiseExternalSource(a_dataRelPath)) {
			// The backend opens this path itself (game-root-relative, USVFS-aware), caches the bytes,
			// and posts via pInMemory — a loose szFile is NEVER opened by the engine's registry-gated
			// audio resolver (RE-proven 1.16.244). make_preferred() = backslashes; 'Data\' is game-root.
			const auto rel = (std::filesystem::path("Data") / a_dataRelPath).make_preferred().wstring();
			if (const auto playingID = Wwise::PostExternalFile(rel)) {
				REX::INFO("SoundService: posted external '{}' -> playingID {} (engine-mixed)",
					a_dataRelPath, playingID);
				return;
			}
			REX::WARN("SoundService: Wwise .wem post rejected '{}' — falling back to the miniaudio device",
				a_dataRelPath);
		}

		// ---- legacy miniaudio fallback (bypasses the game mix; removed once the external-source
		//      path is validated in-game — Milestone 0). Reached only when Wwise is unavailable
		//      (prologue mismatch), the codec isn't directly streamable, or the post was rejected. ----
		Init();  // no-op after the first attempt; normally kPostDataLoad paid this

		std::scoped_lock l{ lock };
		if (!engineReady) {
			return;
		}

		const auto path = (std::filesystem::current_path() / "Data" / a_dataRelPath).string();
		auto active = std::make_unique<ActiveSound>();

		// ASYNC+STREAM: file IO/decode happens on miniaudio's worker path. Do
		// not use MA_SOUND_FLAG_DECODE here; that creates a decode-only sound
		// which opens successfully but is not routed to the engine for playback.
		constexpr ma_uint32 flags = MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_ASYNC;
		if (ma_sound_init_from_file(&g_engine, path.c_str(), flags, nullptr, nullptr, &active->sound) != MA_SUCCESS) {
			REX::WARN("SoundService: cannot open '{}'", path);
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
			REX::WARN("SoundService: failed to start '{}'", path);
			ma_sound_uninit(&active->sound);
			return;
		}
		REX::DEBUG("SoundService: playing '{}' at ({:.2f},{:.2f},{:.2f}) volume {:.2f}",
			a_dataRelPath, a_worldPos.x, a_worldPos.y, a_worldPos.z, a_volume);

		sounds.push_back(std::move(active));
		activeCount.store(static_cast<int>(sounds.size()), std::memory_order_relaxed);
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

		// Listener follows the player; heading drives the pan frame. data
		// fields are plain reads off another thread's state — tolerable for
		// audio, same tolerance the adjust-hotkeys path already accepts.
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
				ma_sound_uninit(&s->sound);
			}
			return done;
		});
		activeCount.store(static_cast<int>(sounds.size()), std::memory_order_relaxed);
	}

	bool SoundService::SetEnabled(bool a_enabled)
	{
		enabled.store(a_enabled, std::memory_order_relaxed);
		if (!a_enabled) {
			StopAll();
		}
		std::scoped_lock l{ lock };
		return a_enabled && (engineReady || !initAttempted);
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
		for (auto& s : sounds) {
			ma_sound_uninit(&s->sound);
		}
		sounds.clear();
		activeCount.store(0, std::memory_order_relaxed);
	}

	void SoundService::RunWwiseSelfTest()
	{
		// Data-relative path to a Wwise .wem (the engine-mixed external-source path — see Play()).
		const auto rel = (std::filesystem::path("Data") / "OSF" / "Sounds" / "testvoice.wem").make_preferred().wstring();
		Wwise::RunSelfTest(rel);
	}
}
