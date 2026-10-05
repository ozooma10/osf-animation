#pragma once

#include "Util/NativeMainThreadQueue.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>

namespace OSF::API
{
	// One browser presentation lifetime. Posting under the same lock preserves
	// lifecycle/command order; commands never retain callback-owned strings.
	class BrowserSessionQueue
	{
	public:
		void Visibility(bool a_visible, std::function<void()> a_task)
		{
			std::lock_guard lock(m_mutex);
			if (m_visible == a_visible) return;
			m_visible = a_visible;
			if (!a_visible) ++m_generation;
			const auto generation = m_generation;
			Util::NativeMainThreadQueue::Post([this, a_visible, generation, task = std::move(a_task)] {
				// A hidden edge must clean up even if a new show is already queued.
				if (!a_visible || Current(generation, true)) task();
			});
		}

		void Reset(std::function<void()> a_cleanup)
		{
			std::lock_guard lock(m_mutex);
			m_visible = false;
			++m_generation;
			Util::NativeMainThreadQueue::Post(std::move(a_cleanup));
		}

		void Post(std::function<void()> a_task, bool a_requiresVisible = true)
		{
			std::lock_guard lock(m_mutex);
			if (a_requiresVisible && !m_visible) return;
			const auto generation = m_generation;
			Util::NativeMainThreadQueue::Post([this, generation, a_requiresVisible, task = std::move(a_task)] {
				if (Current(generation, a_requiresVisible)) task();
			});
		}

	private:
		bool Current(std::uint64_t a_generation, bool a_requiresVisible)
		{
			std::lock_guard lock(m_mutex);
			return m_generation == a_generation && (!a_requiresVisible || m_visible);
		}
		std::mutex m_mutex;
		std::uint64_t m_generation{ 1 };
		bool m_visible{};
	};
}
