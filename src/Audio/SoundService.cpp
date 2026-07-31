#include "Audio/SoundService.h"

#include "Audio/WwiseBackend.h"

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

		// Process-lifetime worker pool. SFSE plugins are not unloaded during play, and deliberately
		// leaking the pool avoids static-destruction races with decoder/Wwise code at process exit.
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
				}
				_cv.notify_one();
				return true;
			}

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
				}
			}

			std::mutex _mutex;
			std::condition_variable _cv;
			std::deque<std::function<void()>> _jobs;
		};
	}

	SoundService& SoundService::GetSingleton()
	{
		static SoundService instance;
		return instance;
	}

	SoundService::PlayTicket SoundService::BeginPlay()
	{
		std::lock_guard l{ lock };
		const auto sequence = nextSequence++;
		if (nextSequence == 0) {
			nextSequence = 1;
		}
		return { teardownEpoch, sequence };
	}

	bool SoundService::TicketCurrent(const PlayTicket& a_ticket)
	{
		std::lock_guard l{ lock };
		return a_ticket.epoch == teardownEpoch;
	}

	bool SoundService::PublishVoice(
		std::uint64_t a_slot, std::uint32_t a_playingID, const PlayTicket& a_ticket)
	{
		std::lock_guard l{ lock };
		if (a_ticket.epoch != teardownEpoch) {
			Wwise::StopVoice(a_playingID);
			return false;
		}

		if (a_slot == 0) {
			unslottedVoices.push_back(a_playingID);
			if (unslottedVoices.size() > kMaxTrackedUnslottedVoices) {
				Wwise::StopVoice(unslottedVoices.front());
				unslottedVoices.pop_front();
			}
			return true;
		}

		if (const auto it = slots.find(a_slot); it != slots.end()) {
			if (it->second.sequence > a_ticket.sequence) {
				Wwise::StopVoice(a_playingID);
				return false;  // a later Play call published first
			}
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
		return true;
	}

	void SoundService::Play(std::uint64_t a_slot, const std::string& a_dataRelPath)
	{
		const PlayTicket ticket = BeginPlay();

		// Baked events need no file work; post immediately through the engine command queue.
		if (const auto eventID = Wwise::ParseEventSpec(a_dataRelPath)) {
			const auto playingID = Wwise::PostEvent(*eventID);
			if (playingID == 0) {
				REX::WARN("[Audio] Wwise rejected '{}' (event 0x{:08X} not in any loaded bank?) — cue skipped", a_dataRelPath, *eventID);
				return;
			}
			if (PublishVoice(a_slot, playingID, ticket)) {
				REX::DEBUG("[Audio] posted Wwise event '{}' (0x{:08X}) -> playingID {} (slot {:#x})",
					a_dataRelPath, *eventID, playingID, a_slot);
			}
			return;
		}

		if (!Wwise::Available() || !Wwise::IsWwiseExternalSource(a_dataRelPath)) {
			REX::WARN("[Audio] no engine path for '{}' (Wwise {}; unsupported codec?) — cue skipped",
				a_dataRelPath, Wwise::Available() ? "available" : "unavailable");
			return;
		}

		const auto rel = (std::filesystem::path("Data") / a_dataRelPath).make_preferred().wstring();
		const bool queued = AudioWorker::GetSingleton().Enqueue(
			[this, ticket, a_slot, rel, path = a_dataRelPath]() {
				if (!TicketCurrent(ticket)) {
					return;  // queued before a world-replacing teardown
				}
				const auto playingID = Wwise::PostExternalFile(rel);
				if (playingID == 0) {
					REX::WARN("[Audio] Wwise external-source post rejected '{}' — cue skipped", path);
					return;
				}
				if (PublishVoice(a_slot, playingID, ticket)) {
					REX::DEBUG("[Audio] posted external '{}' -> playingID {} (engine-mixed, slot {:#x})",
						path, playingID, a_slot);
				}
			});
		if (!queued) {
			REX::WARN("[Audio] background audio queue is full ({} jobs) — '{}' skipped",
				kMaxPendingAudioJobs, a_dataRelPath);
		}
	}

	void SoundService::StopAll()
	{
		std::lock_guard l{ lock };
		if (++teardownEpoch == 0) {
			teardownEpoch = 1;
		}
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