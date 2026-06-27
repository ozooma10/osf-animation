#pragma once

#include <cstdint>
#include <unordered_map>

// Cue-driven sound playback. The ONLY path is the game's own Wwise engine (WwiseBackend.*),
// so everything rides the game mix — master/SFX volume sliders, the pause menu, ducking and busses all apply:
//
// - "event:<WwiseEventName>" / "event:0x<akEventID>" specs:
//	 posted as a BAKED event (events already in a loaded soundbank), engine-mixed, at the listener.
//
// - Plain file paths (the default for pack cues): posted as a Wwise EXTERNAL SOURCE through a shipped event's
//   "External_Source" slot. A loose .wem plays as-is; a .wav/.mp3/.ogg/.flac is decoded to PCM and wrapped in
//   a PCM .wem at runtime (WwiseBackend), so it rides the same engine-mixed path with no soundbank authoring
//   and no bank to load. This is the GAME'S OWN loose voice-file mechanism (RE-proven on 1.16.244).
//	 v1 posts at the listener (player game object); positioned posting is a deferred follow-up.
//
// There is no private-device fallback: a cue the Wwise path can't take (Wwise unavailable, or a codec the
// external source can't stream) is logged and skipped — OSF never plays audio outside the game mix.
//
// Threading: Play() is called from animation job threads (under the scene lock) and must never block.
// The Wwise post only enqueues into AK's command queue (any-thread-safe). Slot state is guarded by one mutex.

namespace OSF::Audio
{
	class SoundService
	{
	public:
		static SoundService& GetSingleton();

		// Playback of a Data-relative file ("OSF/Sounds/x.wav") or "event:" spec. The world position and
		// a_volume are carried for the deferred positioned-posting follow-up; today's listener-centered Wwise
		// posts ignore them (the mix is engine-owned). Safe from any thread; cheap on failure.
		//
		// a_slot is a VOICE-CHANNEL key (per-actor, computed by the caller from the role actor's formID;
		// 0 = unslotted/always layer). When nonzero, a new Play for a slot REPLACES that slot's currently
		// playing voice: the prior clip is cut via Wwise::StopVoice (runtime-proven AK ExecuteActionOnPlayingID
		// — see WwiseBackend; self-disables only on a future patch) so cues on the same channel never overlap.
		// Two different slots play independently.
		void Play(std::uint64_t a_slot, const std::string& a_dataRelPath, const RE::NiPoint3& a_worldPos, float a_volume = 1.0f);

		// Cuts every live voice (GraphManager::StopAll, a loaded save should not have last-world sounds ringing over it).
		// Normal scene teardown deliberately lets sub-second tails finish on their own.
		void StopAll();

	private:
		// caller holds `lock`. Cut the Wwise voice that currently owns a_slot and drop the slot entry.
		// No-op for slot 0 or an unowned slot.
		void ClearSlotLocked(std::uint64_t a_slot);

		std::mutex lock;
		std::unordered_map<std::uint64_t, std::uint32_t> slots;  // voice channel -> live AkPlayingID (guarded by `lock`)
	};
}
