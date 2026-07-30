#pragma once

#include "API/OSFSceneAPI.h"

#include <mutex>
#include <vector>

namespace OSF::API
{
	// Engine-independent storage for the public native callback surface.
	// Dispatch snapshots registrations under the lock and invokes callbacks
	// after releasing it, allowing safe self-unregistration.
	class NativeSceneEventRegistry
	{
	public:
		[[nodiscard]] std::uint64_t Register(
			OSFSceneEventCallback a_callback, void* a_context,
			std::int32_t a_sceneFilter, std::int32_t a_eventMask);
		[[nodiscard]] bool Unregister(std::uint64_t a_token);

		// Returns the number of callbacks that threw. Exceptions are isolated so
		// one faulty consumer cannot prevent later callbacks from running.
		[[nodiscard]] std::size_t Dispatch(const OSFSceneEvent& a_event);

	private:
		struct Entry
		{
			std::uint32_t generation = 0;
			OSFSceneEventCallback callback = nullptr;
			void* context = nullptr;
			std::int32_t sceneFilter = 0;
			std::int32_t eventMask = 0;
		};

		[[nodiscard]] static std::uint64_t MakeToken(
			std::uint32_t a_generation, std::uint32_t a_slot);

		std::mutex _lock;
		std::vector<Entry> _slots;
		std::uint32_t _nextGeneration = 1;
	};
}
