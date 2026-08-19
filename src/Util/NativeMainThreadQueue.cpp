#include "Util/NativeMainThreadQueue.h"

#include "RE/B/BSService.h"

#include <REX/W32/KERNEL32.h>

namespace OSF::Util::NativeMainThreadQueue
{
	namespace
	{
		// The engine owns this delegate after QueueTask steals its reference. If an unexpected
		// thread releases it, fail closed rather than running engine work off the verified drain.
		class GuardedTask final : public RE::BSService::QueuedDelegate
		{
		public:
			GuardedTask(
				std::function<void()> a_task,
				std::string_view a_logLabel,
				std::function<void()> a_onDrop) :
				_task(std::move(a_task)),
				_logLabel(a_logLabel),
				_onDrop(std::move(a_onDrop))
			{}

			void Run() override
			{
				const auto drainOwnerThreadID = RE::BSService::TaskQueue::GetDrainOwnerThreadID();
				const auto currentThreadID = REX::W32::GetCurrentThreadId();
				if (currentThreadID != drainOwnerThreadID) {
					RunDropRecovery();
					REX::CRITICAL("{} dropped on thread {} (drain owner {})", _logLabel, currentThreadID, drainOwnerThreadID);
					return;
				}

				try {
					_task();
				} catch (const std::exception& e) {
					REX::ERROR("{} threw '{}'; payload stopped", _logLabel, e.what());
				} catch (...) {
					REX::ERROR("{} threw an unknown exception; payload stopped", _logLabel);
				}
			}

		private:
			void RunDropRecovery() noexcept
			{
				if (!_onDrop) {
					return;
				}
				try {
					_onDrop();
				} catch (...) {
					// Recovery must never escape the engine queue drain.
				}
			}

			std::function<void()> _task;
			std::string _logLabel;
			std::function<void()> _onDrop;
		};
	}

	QueueState SnapshotState()
	{
		QueueState state;
		state.currentThreadID = REX::W32::GetCurrentThreadId();
		state.drainOwnerThreadID = RE::BSService::TaskQueue::GetDrainOwnerThreadID();

		auto* queue = RE::BSService::TaskQueue::GetSingleton();
		state.singleton = reinterpret_cast<std::uintptr_t>(queue);
		state.queueEnabled = queue && RE::BSService::TaskQueue::IsQueueEnabled();
		state.insideDrain = state.drainOwnerThreadID != 0 && state.currentThreadID == state.drainOwnerThreadID;
		return state;
	}

	bool IsAvailable()
	{
		const auto state = SnapshotState();
		return state.insideDrain || (state.singleton != 0 && state.queueEnabled);
	}

	PostResult Post(
		std::function<void()> a_task,
		const std::string_view a_logLabel,
		std::function<void()> a_onDrop)
	{
		if (SnapshotState().insideDrain) {
			a_task();
			return PostResult::kRanInline;
		}

		auto* queue = RE::BSService::TaskQueue::GetSingleton();
		if (!queue || !RE::BSService::TaskQueue::IsQueueEnabled()) {
			return PostResult::kUnavailable;
		}

		RE::BSService::QueuedDelegate* task =
			new GuardedTask(std::move(a_task), a_logLabel, std::move(a_onDrop));
		queue->QueueTask(task);
		if (!task) {
			return PostResult::kQueued;
		}

		delete task;
		return PostResult::kUnavailable;
	}

	const char* ToString(const PostResult a_result)
	{
		switch (a_result) {
		case PostResult::kQueued:
			return "queued";
		case PostResult::kRanInline:
			return "ran-inline";
		case PostResult::kUnavailable:
			return "unavailable";
		default:
			return "unknown";
		}
	}
}
