#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string_view>
#include <unordered_map>

// Playback only through the game's Wwise engine, so the game mix (volume sliders, pause, ducking) applies.
// "event:" specs post baked events; file paths post as external sources (non-.wem decoded and wrapped as PCM .wem).
// No private-device fallback: a cue Wwise can't take is logged and skipped.
// Play() is any-thread and does no I/O; media prep runs on a bounded worker queue. StopAll invalidates in-flight work by epoch.

namespace OSF::Audio
{
	class SoundService
	{
	public:
		static SoundService& GetSingleton();

		// Returns 0 until a verified engine-managed actor game object exists; playback stays listener-relative.
		std::uint64_t PrepareRoleEmitter(RE::Actor* a_actor);

		// a_slot is a per-actor voice channel (0 = unslotted, layers): a new Play cuts that slot's previous voice.
		// a_gameObject 0 = listener.
		void Play(std::uint64_t a_slot, const std::string& a_dataRelPath, std::uint64_t a_gameObject = 0);

		// Independent, bounded capability handles. Close retires the handle permanently and
		// serializes with submission: no queued job can post after Close returns.
		std::uint64_t CreateChannel();
		bool PlayChannel(std::uint64_t a_channel, const std::string& a_spec);
		bool CloseChannel(std::uint64_t a_channel);
#ifdef OSF_AUDIO_TEST
		bool WaitIdleForTest();
#endif

		// For world replacement (load); normal scene teardown lets short tails finish.
		void StopAll();

#ifdef OSF_ANIMATION_SMOKE
		std::uint32_t VoiceForSmoke(std::uint64_t a_slot)
		{
			std::lock_guard guard(lock);
			const auto it = slots.find(a_slot);
			return it == slots.end() ? 0 : it->second.playingID;
		}
		std::uint32_t ChannelVoiceForSmoke(std::uint64_t a_channel)
		{
			std::lock_guard guard(lock);
			const auto it = channels.find(a_channel);
			return it == channels.end() ? 0 : it->second.playingID;
		}
#endif

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

		[[nodiscard]] PlayTicket BeginPlay(std::uint64_t a_channel);
		// Caller holds lock for these helpers. Commit holds it through post AND publication.
		[[nodiscard]] bool TicketCurrent(std::uint64_t a_slot, std::uint64_t a_channel, const PlayTicket& a_ticket) const;
		void PublishVoice(std::uint64_t a_slot, std::uint32_t a_playingID, const PlayTicket& a_ticket);
		bool Commit(std::uint64_t a_slot, std::uint64_t a_channel, const PlayTicket& a_ticket,
			std::string_view a_spec, const std::function<std::uint32_t()>& a_post);
		bool PlayImpl(std::uint64_t a_slot, std::uint64_t a_channel, const std::string& a_spec, std::uint64_t a_gameObject);

		std::mutex lock;
		std::unordered_map<std::uint64_t, SlotVoice> slots;
		std::unordered_map<std::uint64_t, SlotVoice> channels;
		std::deque<SlotOrderEntry> slotOrder;
		std::deque<std::uint32_t> unslottedVoices;           // stopped on world teardown
		std::uint64_t teardownEpoch = 1;
		std::uint64_t nextSequence = 1;
		std::uint64_t nextChannel = 1;
	};
}
