#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace OSF::Util::NativeMainThreadQueue
{
	enum class PostResult
	{
		kQueued,
		kRanInline,
		kUnavailable,
	};

	struct QueueState
	{
		std::uintptr_t singleton{ 0 };
		std::uint32_t currentThreadID{ 0 };
		std::uint32_t drainOwnerThreadID{ 0 };
		bool queueEnabled{ false };
		bool insideDrain{ false };
	};

	[[nodiscard]] QueueState SnapshotState();
	[[nodiscard]] bool IsAvailable();

	[[nodiscard]] PostResult Post(
		std::function<void()> a_task,
		std::string_view a_logLabel,
		std::function<void()> a_onDrop = {});

	[[nodiscard]] const char* ToString(PostResult a_result);
}
