#include "Audio/SoundService.h"

#include "Audio/WwiseBackend.h"
#include "Testing/D3AudioProbe.h"

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <thread>

namespace OSF::Audio
{
	namespace
	{
		constexpr std::size_t kAudioWorkerCount = 2;
		constexpr std::size_t kMaxPendingAudioJobs = 256;
		constexpr std::size_t kMaxTrackedUnslottedVoices = 256;
		constexpr std::size_t kMaxTrackedSlottedVoices = 4096;
		constexpr std::size_t kMaxSlotOrderRecords = 8192;

		// Leaked on purpose: avoids static-destruction races with decoder/Wwise code at process exit.
		class AudioWorker
		{
		public:
			static AudioWorker& GetSingleton()
			{
				static auto* instance = new AudioWorker();
				return *instance;
			}

			bool Enqueue(std::function<void()> a_job)
			{
				{
					std::lock_guard lock{ _mutex };
					if (_jobs.size() >= kMaxPendingAudioJobs) {
						return false;
					}
					_jobs.push_back(std::move(a_job));
#ifdef OSF_AUDIO_TEST
					++_pending;
#endif
				}
				_cv.notify_one();
				return true;
			}

#ifdef OSF_AUDIO_TEST
			bool WaitIdle()
			{
				std::unique_lock l{ _mutex };
				return _idle.wait_for(l, std::chrono::seconds(5), [&] { return _pending == 0; });
			}
#endif

		private:
			AudioWorker()
			{
				for (std::size_t i = 0; i < kAudioWorkerCount; ++i) {
					std::thread([this] { Run(); }).detach();
				}
			}

			void Run()
			{
				for (;;) {
					std::function<void()> job;
					{
						std::unique_lock lock{ _mutex };
						_cv.wait(lock, [&] { return !_jobs.empty(); });
						job = std::move(_jobs.front());
						_jobs.pop_front();
					}
					try {
						job();
					} catch (const std::exception& e) {
						REX::ERROR("[Audio] background audio job failed: {}", e.what());
					} catch (...) {
						REX::ERROR("[Audio] background audio job failed with an unknown exception");
					}
#ifdef OSF_AUDIO_TEST
					{ std::lock_guard l{ _mutex }; --_pending; }
					_idle.notify_all();
#endif
				}
			}

			std::mutex _mutex;
			std::condition_variable _cv;
			std::deque<std::function<void()>> _jobs;
#ifdef OSF_AUDIO_TEST
			std::condition_variable _idle;
			std::size_t _pending{};
#endif
		};
	}

	SoundService& SoundService::GetSingleton()
	{
		// Leaked: detached workers capture it.
		static auto* instance = new SoundService();
		return *instance;
	}
#ifdef OSF_AUDIO_TEST
	bool SoundService::WaitIdleForTest() { return AudioWorker::GetSingleton().WaitIdle(); }
#endif

	std::uint64_t SoundService::PrepareRoleEmitter(RE::Actor* a_actor)
	{
		(void)a_actor;
		return 0;
	}

	std::uint64_t SoundService::CreateChannel()
	{
		std::lock_guard l{ lock };
		if (!nextChannel || channels.size() >= kMaxTrackedSlottedVoices) return 0;
		const auto id = nextChannel++;
		channels.emplace(id, SlotVoice{});
		return id;
	}

	bool SoundService::CloseChannel(std::uint64_t a_channel)
	{
		std::lock_guard l{ lock };
		const auto it = channels.find(a_channel);
		if (it == channels.end()) return false;
		if (it->second.playingID) Wwise::StopVoice(it->second.playingID);
		channels.erase(it);
		return true;
	}

	bool SoundService::PlayChannel(std::uint64_t a_channel, const std::string& a_spec)
	{
		if (!a_channel || a_spec.empty() || a_spec.size() > 4096 || a_spec.front() == '$') return false;
		if (!a_spec.starts_with("event:")) {
			const std::filesystem::path path(a_spec);
			if (path.has_root_path()) return false;
			for (const auto& part : path) if (part == "..") return false;
		}
		return PlayImpl(0, a_channel, a_spec, 0);
	}

	SoundService::PlayTicket SoundService::BeginPlay(std::uint64_t a_channel)
	{
		std::lock_guard l{ lock };
		if (!nextSequence || (a_channel && !channels.contains(a_channel))) return {};
		const auto sequence = nextSequence++;
		return { teardownEpoch, sequence };
	}

	bool SoundService::TicketCurrent(std::uint64_t a_slot, std::uint64_t a_channel, const PlayTicket& a_ticket) const
	{
		if (!a_ticket.sequence || a_ticket.epoch != teardownEpoch) return false;
		if (a_channel) {
			const auto it = channels.find(a_channel);
			return it != channels.end() && it->second.sequence <= a_ticket.sequence;
		}
		const auto it = slots.find(a_slot);
		return !a_slot || it == slots.end() || it->second.sequence <= a_ticket.sequence;
	}

	bool SoundService::Commit(std::uint64_t a_slot, std::uint64_t a_channel, const PlayTicket& a_ticket,
		const std::function<std::uint32_t()>& a_post)
	{
		// Preparation must finish before this lock. Close/StopAll cannot interleave the
		// final validity check, Wwise submission and publication; no user callbacks run here.
		std::lock_guard l{ lock };
		if (!TicketCurrent(a_slot, a_channel, a_ticket)) {
#ifdef OSF_ANIMATION_SMOKE
			Testing::D3AudioProbe::Point(Testing::D3AudioProbe::Current(), "cancelledBeforePostNs");
#endif
			return false;
		}
		const auto playing = a_post();
		if (!playing) return false;
		if (a_channel) {
			auto& voice = channels.at(a_channel);
			if (voice.playingID) Wwise::StopVoice(voice.playingID);
			voice.playingID = playing;
			voice.sequence = a_ticket.sequence;
		} else PublishVoice(a_slot, playing, a_ticket);
#ifdef OSF_ANIMATION_SMOKE
		Testing::D3AudioProbe::Point(Testing::D3AudioProbe::Current(), "publishedNs");
#endif
		return true;
	}

	void SoundService::PublishVoice(
		std::uint64_t a_slot, std::uint32_t a_playingID, const PlayTicket& a_ticket)
	{
		if (a_slot == 0) {
			unslottedVoices.push_back(a_playingID);
			if (unslottedVoices.size() > kMaxTrackedUnslottedVoices) {
				Wwise::StopVoice(unslottedVoices.front());
				unslottedVoices.pop_front();
			}
			return;
		}

		if (const auto it = slots.find(a_slot); it != slots.end()) {
			if (it->second.playingID != 0) {
				Wwise::StopVoice(it->second.playingID);
			}
		}
		slots[a_slot] = SlotVoice{ a_ticket.sequence, a_playingID };
		slotOrder.push_back({ a_slot, a_ticket.sequence });
		if (slotOrder.size() > kMaxSlotOrderRecords) {
			std::erase_if(slotOrder, [&](const SlotOrderEntry& a_entry) {
				const auto it = slots.find(a_entry.slot);
				return it == slots.end() || it->second.sequence != a_entry.sequence;
			});
		}
		while (slots.size() > kMaxTrackedSlottedVoices && !slotOrder.empty()) {
			const auto oldest = slotOrder.front();
			slotOrder.pop_front();
			const auto it = slots.find(oldest.slot);
			if (it == slots.end() || it->second.sequence != oldest.sequence) {
				continue;
			}
			Wwise::StopVoice(it->second.playingID);
			slots.erase(it);
		}
	}

	void SoundService::Play(std::uint64_t a_slot, const std::string& a_dataRelPath, std::uint64_t a_gameObject)
	{
		(void)PlayImpl(a_slot, 0, a_dataRelPath, a_gameObject);
	}

	bool SoundService::PlayImpl(std::uint64_t a_slot, std::uint64_t a_channel, const std::string& a_dataRelPath, std::uint64_t a_gameObject)
	{
		const auto eventID = Wwise::ParseEventSpec(a_dataRelPath);
		if (a_dataRelPath.starts_with("event:") && !eventID) return false;
		if (!eventID && !Wwise::IsWwiseExternalSource(a_dataRelPath)) {
			REX::WARN("[Audio] unsupported sound spec '{}' — cue skipped", a_dataRelPath);
			return false;
		}
		const PlayTicket ticket = BeginPlay(a_channel);
		if (!ticket.sequence) return false;

		if (eventID) {
			return Commit(a_slot, a_channel, ticket, [&] { return Wwise::PostEvent(*eventID, a_gameObject); });
		}

		const auto rel = (std::filesystem::path("Data") / a_dataRelPath).make_preferred().wstring();
#ifdef OSF_ANIMATION_SMOKE
		const auto d3 = Testing::D3AudioProbe::CaptureAudio(a_dataRelPath, a_slot);
		if (a_channel) Testing::D3AudioProbe::Point(d3, "channel", a_channel);
#endif
		const bool queued = AudioWorker::GetSingleton().Enqueue(
			[this, ticket, a_slot, a_channel, a_gameObject, rel, path = a_dataRelPath
#ifdef OSF_ANIMATION_SMOKE
			, d3
#endif
			]() {
#ifdef OSF_ANIMATION_SMOKE
				Testing::D3AudioProbe::AudioContext context(d3);
				Testing::D3AudioProbe::Point(d3, "workerNs");
				Testing::D3AudioProbe::Checkpoint(d3, "worker");
#endif
				{
					std::lock_guard l{ lock };
					if (!TicketCurrent(a_slot, a_channel, ticket)) {
#ifdef OSF_ANIMATION_SMOKE
						Testing::D3AudioProbe::Point(d3, "cancelledBeforePrepareNs");
#endif
						return;
					}
				}
				const auto media = Wwise::PrepareExternalFile(rel);
				if (!media.data) {
					REX::WARN("[Audio] media preparation failed '{}' — cue skipped", path);
					return;
				}
				(void)Commit(a_slot, a_channel, ticket, [&] { return Wwise::PostPreparedFile(media, a_gameObject); });
			});
		if (!queued) {
#ifdef OSF_ANIMATION_SMOKE
			Testing::D3AudioProbe::AudioContext context(d3);
			Testing::D3AudioProbe::Point(d3, "queueRejectedNs");
#endif
			REX::WARN("[Audio] background audio queue is full ({} jobs) — '{}' skipped",
				kMaxPendingAudioJobs, a_dataRelPath);
		}
		return queued;
	}

	void SoundService::StopAll()
	{
		std::lock_guard l{ lock };
		if (++teardownEpoch == 0) {
			teardownEpoch = 1;
		}
		for (const auto& [id, voice] : channels) {
			(void)id;
			if (voice.playingID) Wwise::StopVoice(voice.playingID);
		}
		channels.clear();
		for (const auto& [key, voice] : slots) {
			(void)key;
			if (voice.playingID != 0) {
				Wwise::StopVoice(voice.playingID);
			}
		}
		slots.clear();
		slotOrder.clear();
		for (const auto playingID : unslottedVoices) {
			if (playingID != 0) {
				Wwise::StopVoice(playingID);
			}
		}
		unslottedVoices.clear();
	}
}
