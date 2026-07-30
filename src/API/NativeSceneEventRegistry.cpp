#include "API/NativeSceneEventRegistry.h"

#include <limits>

namespace OSF::API
{
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
			if (_slots[slot].generation == 0) {
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

		_slots[slot] = Entry{
			.generation = generation,
			.callback = a_callback,
			.context = a_context,
			.sceneFilter = a_sceneFilter,
			.eventMask = a_eventMask == 0 ? SceneEventType::kAll : a_eventMask
		};
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

		std::lock_guard lock{ _lock };
		if (slot >= _slots.size() ||
			_slots[slot].generation != generation) {
			return false;
		}
		_slots[slot] = {};
		return true;
	}

	std::size_t NativeSceneEventRegistry::Dispatch(
		const OSFSceneEvent& a_event)
	{
		struct Target
		{
			OSFSceneEventCallback callback;
			void* context;
		};
		std::vector<Target> targets;
		{
			std::lock_guard lock{ _lock };
			for (const auto& entry : _slots) {
				if (entry.generation == 0 || !entry.callback ||
					(entry.eventMask & a_event.eventType) == 0 ||
					(entry.sceneFilter != 0 &&
					 entry.sceneFilter != a_event.sceneHandle)) {
					continue;
				}
				targets.push_back({ entry.callback, entry.context });
			}
		}

		std::size_t failures = 0;
		for (const auto& target : targets) {
			try {
				target.callback(&a_event, target.context);
			} catch (...) {
				++failures;
			}
		}
		return failures;
	}
}
