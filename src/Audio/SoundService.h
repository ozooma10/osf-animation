#pragma once

#include <cstdint>
#include <deque>
#include <unordered_map>

// Cue-driven sound playback. The ONLY path is the game's own Wwise engine (WwiseBackend.*),
// so everything rides the game mix — master/SFX volume sliders, the pause menu, ducking and busses all apply:
//
// - "event:<WwiseEventName>" / "event:0x<akEventID>" specs:
//	 posted as a BAKED event (events already in a loaded soundbank), engine-mixed.
//
// - Plain file paths (the default for pack cues): posted as a Wwise EXTERNAL SOURCE through a shipped event's
//   "External_Source" slot. A loose .wem plays as-is; a .wav/.mp3/.ogg/.flac is decoded to PCM and wrapped in
//   a PCM .wem at runtime (WwiseBackend), so it rides the same engine-mixed path with no soundbank authoring
//   and no bank to load. This is the GAME'S OWN loose voice-file mechanism (RE-proven on 1.16.244).
//   Authored emitter:"role" cues retain listener playback until a verified engine-managed actor
//   game-object lookup is available; omitted/listener cues use that same safe path.
//
// There is no private-device fallback: a cue the Wwise path can't take (Wwise unavailable, or a codec the
// external source can't stream) is logged and skipped — OSF never plays audio outside the game mix.
//
// Threading: Play() is safe from any thread and returns without doing file I/O or decoding. External
// media preparation runs on a bounded worker queue; Wwise posting is any-thread-safe. Slot and teardown
// state are guarded by one mutex, and StopAll invalidates queued/in-flight work with an epoch.

namespace OSF::Audio
{
	class SoundService
	{
	public:
		static SoundService& GetSingleton();

		// Returns a verified engine-owned role emitter when available. The current backend deliberately
		// returns 0, preserving listener playback instead of inventing/registering Wwise object IDs.
		std::uint64_t PrepareRoleEmitter(RE::Actor* a_actor);

		// Playback of a Data-relative file ("OSF/Sounds/x.wav") or "event:" spec. It posts on the
		// supplied Wwise game object, or the listener when none is supplied. Role selection and subtitle
		// attribution live above this service. Safe from any thread; cheap on failure.
		//
		// a_slot is a VOICE-CHANNEL key (per-actor, computed by the caller from the role actor's formID;
		// 0 = unslotted/always layer). When nonzero, a new Play for a slot REPLACES that slot's currently
		// playing voice: the prior clip is cut via Wwise::StopVoice (runtime-proven AK ExecuteActionOnPlayingID
		// — see WwiseBackend; self-disables only on a future patch) so cues on the same channel never overlap.
		// Two different slots play independently.
		// a_gameObject 0 retains listener-relative playback.
		void Play(std::uint64_t a_slot, const std::string& a_dataRelPath, std::uint64_t a_gameObject = 0);

		// Cuts every live voice (GraphManager::StopAll, a loaded save should not have last-world sounds ringing over it).
		// Normal scene teardown deliberately lets sub-second tails finish on their own.
		void StopAll();

	private:
		struct PlayTicket
		{
			std::uint64_t epoch = 0;
			std::uint64_t sequence = 0;
		};

		struct SlotVoice
		{
			std::uint64_t sequence = 0;
			std::uint32_t playingID = 0;
		};

		struct SlotOrderEntry
		{
			std::uint64_t slot = 0;
			std::uint64_t sequence = 0;
		};

		[[nodiscard]] PlayTicket BeginPlay();
		[[nodiscard]] bool TicketCurrent(const PlayTicket& a_ticket);
		[[nodiscard]] bool PublishVoice(std::uint64_t a_slot, std::uint32_t a_playingID, const PlayTicket& a_ticket);

		std::mutex lock;
		std::unordered_map<std::uint64_t, SlotVoice> slots;  // bounded channel -> newest live voice
		std::deque<SlotOrderEntry> slotOrder;                // lazy FIFO for bounded channel eviction
		std::deque<std::uint32_t> unslottedVoices;           // bounded layered voices, stopped on world teardown
		std::uint64_t teardownEpoch = 1;
		std::uint64_t nextSequence = 1;
	};
}
