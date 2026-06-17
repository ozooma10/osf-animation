#pragma once

// Cue-driven sound playback, two backends behind one Play() call:
//
// - File paths (the default): our own output device (miniaudio), BYPASSING
//   the game's Wwise mix. Pack cues carry Data-relative sound paths
//   (cues[].sound); the scene fires them here at cue time with the cue
//   actor's rendered world position, and the per-frame Tick keeps the
//   listener on the player. Spatialization is miniaudio's (inverse-distance
//   attenuation + pan from the player's heading) — good enough for short
//   voice/SFX clips, but the sounds do NOT react to the game's volume
//   sliders, pause menu, or ducking.
// - "event:<WwiseEventName>" / "event:0x<akEventID>" specs: posted through
//   the game's OWN Wwise engine (WwiseBackend.*) — engine-mixed, but only
//   events already in the game's loaded soundbanks, and for now unpositioned.
//
// Loose custom files cannot ride the Wwise path (no LoadBank mapping yet),
// so miniaudio stays the backend for pack-shipped audio.
//
// Threading: Play() is called from animation job threads (under the scene
// lock) and must never block — sounds are created with async decode and all
// service state sits behind one mutex. Init() runs the (slow) device init at
// kPostDataLoad so the first cue doesn't stall a job thread.

namespace OSF::Audio
{
	class SoundService
	{
	public:
		static SoundService& GetSingleton();

		// Initializes the output device + engine. Failure logs once and leaves
		// the service disabled (Play becomes a no-op) — audio is a cosmetic
		// feature and must never take the plugin down.
		void Init();

		// Fire-and-forget playback of a Data-relative file ("OSF/Sounds/x.wav")
		// at a world position (meters). a_volume is a per-cue multiplier on top
		// of the master volume. Safe from any thread; cheap on failure.
		void Play(const std::string& a_dataRelPath, const RE::NiPoint3& a_worldPos, float a_volume = 1.0f);

		// Per-frame upkeep, called from the graph update hook: moves the
		// listener to the player and reaps finished sounds. Early-outs on an
		// atomic when nothing is playing, so riding the ~7x/frame update-call
		// stream costs nothing in the idle case.
		void Tick();

		// Master toggle/volume (OSF.SetSoundEnabled / OSF.SetSoundVolume).
		// Returns the EFFECTIVE state — false when the device failed to init.
		bool SetEnabled(bool a_enabled);
		void SetVolume(float a_volume);

		// Reflects the user's master sound toggle. osf.voice.play and the sound lane quietly
		// skip when this is off. (Play() also guards internally — this just lets callers
		// log and skip the setup.)
		bool Enabled() const { return enabled.load(std::memory_order_relaxed); }

		// Cuts every live sound (GraphManager::StopAll — a loaded save should
		// not have last-world sounds ringing over it). Normal scene teardown
		// deliberately lets sub-second tails finish on their own.
		void StopAll();

	private:
		struct ActiveSound;

		// caller holds `lock`
		void ReapLocked();

		std::mutex lock;
		std::vector<std::unique_ptr<ActiveSound>> sounds;
		std::atomic<int> activeCount{ 0 };
		std::atomic<bool> enabled{ true };
		bool engineReady = false;
		bool initAttempted = false;
	};
}
