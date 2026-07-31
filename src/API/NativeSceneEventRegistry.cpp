#include "API/NativeSceneEventRegistry.h"

#include <limits>

namespace OSF::API
{
	namespace
	{
		struct ActiveCallback
		{
			const void* entry = nullptr;
			ActiveCallback* previous = nullptr;
		};

		thread_local ActiveCallback* g_activeCallback = nullptr;

		class ActiveCallbackScope
		{
		public:
			explicit ActiveCallbackScope(const void* a_entry) :
				_frame{ .entry = a_entry, .previous = g_activeCallback }
			{
				g_activeCallback = &_frame;
			}

			~ActiveCallbackScope()
			{
				g_activeCallback = _frame.previous;
			}

		private:
			ActiveCallback _frame;
		};

		[[nodiscard]] std::size_t ActiveCountOnThisThread(const void* a_entry)
		{
			std::size_t count = 0;
			for (auto* frame = g_activeCallback; frame; frame = frame->previous) {
				if (frame->entry == a_entry) {
					++count;
				}
			}
			return count;
		}
	}
	std::uint64_t NativeSceneEventRegistry::MakeToken(
		std::uint32_t a_generation, std::uint32_t a_slot)
	{
		// Low word stores slot + 1 so token zero is always reserved.
		return (static_cast<std::uint64_t>(a_generation) << 32) |
		       (static_cast<std::uint64_t>(a_slot) + 1);
	}

	std::uint64_t NativeSceneEventRegistry::Register(
		OSFSceneEventCallback a_callback, void* a_context,
		std::int32_t a_sceneFilter, std::int32_t a_eventMask)
	{
		if (!a_callback) {
			return 0;
		}

		std::lock_guard lock{ _lock };
		std::size_t slot = 0;
		for (; slot < _slots.size(); ++slot) {
			if (!_slots[slot]) {
				break;
			}
		}
		if (slot == _slots.size()) {
			if (slot >= std::numeric_limits<std::uint32_t>::max()) {
				return 0;
			}
			_slots.emplace_back();
		}

		const auto generation = _nextGeneration++;
		if (_nextGeneration == 0) {
			_nextGeneration = 1;
		}

		auto entry = std::make_shared<Entry>();
		entry->generation = generation;
		entry->callback = a_callback;
		entry->context = a_context;
		entry->sceneFilter = a_sceneFilter;
		entry->eventMask = a_eventMask == 0 ? SceneEventType::kAll : a_eventMask;
		_slots[slot] = std::move(entry);
		return MakeToken(generation, static_cast<std::uint32_t>(slot));
	}

	bool NativeSceneEventRegistry::Unregister(std::uint64_t a_token)
	{
		const auto encodedSlot = static_cast<std::uint32_t>(a_token);
		const auto generation = static_cast<std::uint32_t>(a_token >> 32);
		if (encodedSlot == 0 || generation == 0) {
			return false;
		}
		const auto slot = encodedSlot - 1;

		std::shared_ptr<Entry> entry;
		{
			std::lock_guard lock{ _lock };
			if (slot >= _slots.size() || !_slots[slot] ||
				_slots[slot]->generation != generation) {
				return false;
			}
			entry = std::move(_slots[slot]);
		}

		const auto activeHere = ActiveCountOnThisThread(entry.get());
		std::unique_lock lifetimeLock{ entry->lifetimeMutex };
		entry->active = false;
		entry->lifetimeCV.wait(lifetimeLock, [&] {
			// A callback may unregister itself (including through nested dispatch),
			// but it still waits for every invocation running on another thread.
			return entry->inFlight <= activeHere;
		});
		return true;
	}

	std::size_t NativeSceneEventRegistry::Dispatch(
		const OSFSceneEvent& a_event)
	{
		std::vector<std::shared_ptr<Entry>> targets;
		{
			std::lock_guard lock{ _lock };
			for (const auto& entry : _slots) {
				if (!entry || !entry->callback ||
					(entry->eventMask & a_event.eventType) == 0 ||
					(entry->sceneFilter != 0 &&
					 entry->sceneFilter != a_event.sceneHandle)) {
					continue;
				}
				targets.push_back(entry);
			}
		}

		std::size_t failures = 0;
		for (const auto& entry : targets) {
			{
				std::lock_guard lifetimeLock{ entry->lifetimeMutex };
				if (!entry->active) {
					continue;
				}
				++entry->inFlight;
			}

			ActiveCallbackScope activeScope{ entry.get() };
			try {
				entry->callback(&a_event, entry->context);
			} catch (...) {
				++failures;
			}

			{
				std::lock_guard lifetimeLock{ entry->lifetimeMutex };
				--entry->inFlight;
				entry->lifetimeCV.notify_all();
			}
		}
		return failures;
	}
}