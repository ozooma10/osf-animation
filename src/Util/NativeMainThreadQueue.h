#pragma once

#include <functional>
#include <vector>

namespace OSF::Util::NativeMainThreadQueue
{
	// Any thread. Always deferred, including posts made by a command during Drain().
	void Post(std::function<void()> a_task);

	// Order preserved within one frame batch.
	void PostBatch(std::vector<std::function<void()>> a_tasks);

	// Frame-hook owner only. Runs one finite batch without holding the queue lock.
	void Drain();
}
