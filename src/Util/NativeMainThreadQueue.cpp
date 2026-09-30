#include "Util/NativeMainThreadQueue.h"

#include <iterator>
#include <mutex>
#include <utility>

namespace OSF::Util::NativeMainThreadQueue
{
	namespace
	{
		std::mutex g_lock;
		std::vector<std::function<void()>> g_pending;
	}

	void Post(std::function<void()> a_task)
	{
		std::lock_guard lock{ g_lock };
		g_pending.push_back(std::move(a_task));
	}

	void PostBatch(std::vector<std::function<void()>> a_tasks)
	{
		if (a_tasks.empty()) return;
		std::lock_guard lock{ g_lock };
		g_pending.insert(g_pending.end(),
			std::make_move_iterator(a_tasks.begin()), std::make_move_iterator(a_tasks.end()));
	}

	void Drain()
	{
		std::vector<std::function<void()>> batch;
		{
			std::lock_guard lock{ g_lock };
			batch.swap(g_pending);
		}
		for (auto& task : batch) {
			task();
		}
	}
}
