#pragma once
#include "Util/NativeMainThreadQueue.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace OSF::API
{
	// Engine notification state only. Holding the recursive lock through dispatch permits self-removal
	// while making removal from another thread wait for its callback to finish.
	class EngineServiceState
	{
	public:
		using Callback = std::function<void(std::uint32_t)>;
		std::uint64_t Subscribe(Callback callback)
		{
			if (!callback) return 0;
			std::lock_guard lock(m_mutex);
			const auto token = ++m_next;
			m_callbacks.emplace(token, std::make_shared<Callback>(std::move(callback)));
			return token;
		}
		bool Unsubscribe(std::uint64_t token)
		{
			std::lock_guard lock(m_mutex);
			return m_callbacks.erase(token) != 0;
		}
		void Notify(std::uint32_t event)
		{
			std::lock_guard lock(m_mutex);
			const auto callbacks = m_callbacks;
			for (const auto& [token, callback] : callbacks) {
				if (m_callbacks.contains(token)) { try { (*callback)(event); } catch (...) {} }
			}
		}
		bool Post(void (*callback)(void*, bool), void* context)
		{
			if (!callback) return false;
			try {
				const auto epoch = Epoch();
				Util::NativeMainThreadQueue::Post([=, this] { try { callback(context, epoch == Epoch()); } catch (...) {} });
				return true;
			} catch (...) { return false; }
		}
		std::uint64_t Epoch() const { return m_epoch.load(std::memory_order_acquire); }
		void ResetWorld() { m_epoch.fetch_add(1, std::memory_order_acq_rel); Notify(1); }
	private:
		std::recursive_mutex m_mutex;
		std::map<std::uint64_t, std::shared_ptr<Callback>> m_callbacks;
		std::uint64_t m_next{};
		std::atomic<std::uint64_t> m_epoch{1};
	};
}
