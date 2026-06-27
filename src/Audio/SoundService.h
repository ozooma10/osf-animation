#pragma once

#include <cstdint>
#include <unordered_map>

// Cue-driven sound playback. The single intended path is the game's own Wwise engine (WwiseBackend.*)
// so everything rides the game mixmaster/SFX volume sliders, the pause menu, ducking and busses all apply:

// - "event:<WwiseEventName>" / "event:0x<akEventID>" specs: 
//	 posted as a BAKED event (events already in a loaded soundbank), engine-mixed, at the listener.

// - Plain file paths (the default for pack cues): posted as a Wwise EXTERNAL SOURCE through a shipped event's "External_Source" slot, loose .wav/.wem/.xwm
//   play engine-mixed, no per-file soundbank authoring and no bank to load. This is the GAME'S OWN loose voice-file mechanism (RE-proven on 1.16.244). 
//	 v1 posts at the listener (player game object); positioned posting is a deferred follow-up.

// LEGACY FALLBACK (miniaudio, to be removed once the Wwise path is validated in-game): 
// when Wwise is unavailable, or for a codec the external source can't stream directly, 
// a plain file still plays through our own output device, BYPASSING the game mix. 

// Threading: Play() is called from animation job threads (under the scene lock) and must never block. 
// The Wwise post only enqueues into AK's command queue (any-thread-safe). 
// The miniaudio fallback creates sounds async and keeps all service state behind one mutex. 
// Init() runs the (slow) device init at kPostDataLoad so the first cue doesn't stall a job thread.

namespace OSF::Audio
{
	class SoundService
	{
	public:
		static SoundService& GetSingleton();

		// Initializes the output device + engine. Failure logs once and leaves the service disabled
		void Init();

		// Playback of a Data-relative file ("OSF/Sounds/x.wav") at a world position (meters).
		// a_volume is a per-cue multiplier on top of the master volume. Safe from any thread; cheap on failure.
		//
		// a_slot is a VOICE-CHANNEL key (per-actor, computed by the caller from the role actor's formID;
		// 0 = unslotted/always layer). When nonzero, a new Play for a slot REPLACES that slot's currently
		// playing sound: the prior clip is cut so cues on the same channel never overlap. The miniaudio
		// fallback stops the prior sound outright; the Wwise path cuts the prior voice via Wwise::StopVoice
		// (runtime-proven AK ExecuteActionOnPlayingID — see WwiseBackend; self-disables only on a future patch).
		// Two different slots play independently.
		void Play(std::uint64_t a_slot, const std::string& a_dataRelPath, const RE::NiPoint3& a_worldPos, float a_volume = 1.0f);

		// Per-frame upkeep, called from the graph update hook: moves the listener to the player and reaps finished sounds. 
		// Early-outs on an atomic when nothing is playing, so riding the ~7x/frame update-call stream costs nothing in the idle case.
		void Tick();

		// Master cue volume, clamped 0..2.
		void SetVolume(float a_volume);

		// Cuts every live sound (GraphManager::StopAll, a loaded save should not have last-world sounds ringing over it). 
		// Normal scene teardown deliberately lets sub-second tails finish on their own.
		// (Only reaches the miniaudio fallback sounds; Wwise posts are fire-and-forget short cues the engine owns.)
		void StopAll();

	private:
		struct ActiveSound;

		// What currently owns a voice slot: a non-owning pointer to the live miniaudio sound (it lives in
		// `sounds`) and/or the AkPlayingID of the live Wwise voice. Either may be unset across replaces.
		struct SlotOwner
		{
			ActiveSound*  miniSound = nullptr;  // non-owning; owned by `sounds`
			std::uint32_t wwisePlayingID = 0;   // 0 = none
		};

		// caller holds `lock`
		void ReapLocked();

		// caller holds `lock`. Cut whatever currently owns a_slot (its miniaudio sound and/or Wwise voice)
		// and drop the slot entry. No-op for slot 0 or an unowned slot.
		void ClearSlotLocked(std::uint64_t a_slot);

		std::mutex lock;
		std::vector<std::unique_ptr<ActiveSound>> sounds;
		std::unordered_map<std::uint64_t, SlotOwner> slots;  // voice channel -> current owner (guarded by `lock`)
		std::atomic<int> activeCount{ 0 };
		bool engineReady = false;
		bool initAttempted = false;
	};
}
