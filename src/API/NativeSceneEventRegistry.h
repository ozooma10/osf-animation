#pragma once

#include "API/OSFSceneAPI.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

namespace OSF::API
{
	// Callbacks run outside the lock; Unregister waits for in-flight calls on other threads, so self-unregistration is safe.
	class NativeSceneEventRegistry
	{
	public:
		[[nodiscard]] std::uint64_t Register(
			OSFSceneEventCallback a_callback, void* a_context,
			std::int32_t a_sceneFilter, std::int32_t a_eventMask);
		[[nodiscard]] bool Unregister(std::uint64_t a_token);

		// Returns the number of callbacks that threw; a throwing consumer doesn't block the rest.
		[[nodiscard]] std::size_t Dispatch(const OSFSceneEvent& a_event);

	private:
		struct Entry
		{
			std::uint32_t generation = 0;
			OSFSceneEventCallback callback = nullptr;
			void* context = nullptr;
			std::int32_t sceneFilter = 0;
			std::int32_t eventMask = 0;

			std::mutex lifetimeMutex;
			std::condition_variable lifetimeCV;
			bool active = true;
			std::uint32_t inFlight = 0;
		};

		[[nodiscard]] static std::uint64_t MakeToken(
			std::uint32_t a_generation, std::uint32_t a_slot);

		std::mutex _lock;
		std::vector<std::shared_ptr<Entry>> _slots;
		std::uint32_t _nextGeneration = 1;
	};
}