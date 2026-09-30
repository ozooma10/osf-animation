#pragma once

#include <functional>
#include <vector>

namespace OSF::Util::NativeMainThreadQueue
{
	// Any thread. Always deferred, including posts made by a command during Drain().
	void Post(std::function<void()> a_task);

	// Publish related commands together, preserving their order within one frame batch.
	void PostBatch(std::vector<std::function<void()>> a_tasks);

	// Frame-hook owner only. Take one finite batch and execute it without holding the queue lock.
	void Drain();
}
