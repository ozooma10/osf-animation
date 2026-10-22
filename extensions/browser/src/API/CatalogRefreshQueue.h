#pragma once
#include "OSFCatalog.h"
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace OSF::API::EngineClient
{
	// One metadata-only worker. Requests coalesce; superseded results never publish.
	// Only the frame owner calls Publish. Old snapshots are destroyed on the worker.
	class CatalogRefreshQueue
	{
	public:
		using Snapshot = Catalog::Snapshot;
		struct Completion { bool success{}, scan{}; std::uint64_t revision{}; };
		CatalogRefreshQueue(std::function<std::unique_ptr<Snapshot>()> load, std::function<void()> notify) :
			m_load(std::move(load)), m_notify(std::move(notify)),
			m_worker([this](std::stop_token stop) { Run(stop); }) {}
		void Request(bool scan = false)
		{
			std::scoped_lock lock{m_lock};
			++m_requested;
			m_scanRequested |= scan;
			m_wake.notify_one();
		}
		std::optional<Completion> Publish(std::unique_ptr<Snapshot>& current)
		{
			std::scoped_lock lock{m_lock};
			if (m_completed != m_requested || m_completed == m_consumed) return {};
			const bool success = m_ready != nullptr;
			if (success) {
				m_retired.push_back(std::move(current));
				current = std::move(m_ready);
				m_scanRequested = false;
				m_wake.notify_one();
			}
			m_consumed = m_completed;
			return Completion{success, success && m_scanReady, m_completed};
		}
	private:
		void Run(std::stop_token stop)
		{
			std::uint64_t attempted{};
			for (;;) {
				std::vector<std::unique_ptr<Snapshot>> retired;
				std::uint64_t revision{};
				{
					std::unique_lock lock{m_lock};
					if (!m_wake.wait(lock, stop, [&] { return m_requested != attempted || !m_retired.empty(); })) return;
					retired.swap(m_retired);
					revision = m_requested;
				}
				retired.clear();
				if (revision == attempted) continue;
				std::unique_ptr<Snapshot> result;
				try { result = m_load(); } catch (...) {} // Keep the last good copy; the next request retries.
				attempted = revision;
				std::unique_ptr<Snapshot> superseded;
				{
					std::scoped_lock lock{m_lock};
					if (revision != m_requested) continue;
					superseded = std::move(m_ready);
					m_ready = std::move(result);
					m_completed = revision;
					m_scanReady = m_scanRequested;
				}
				try { m_notify(); } catch (...) {}
			}
		}
		std::mutex m_lock;
		std::condition_variable_any m_wake;
		std::uint64_t m_requested{}, m_completed{}, m_consumed{};
		bool m_scanRequested{}, m_scanReady{};
		std::unique_ptr<Snapshot> m_ready;
		std::vector<std::unique_ptr<Snapshot>> m_retired;
		std::function<std::unique_ptr<Snapshot>()> m_load;
		std::function<void()> m_notify;
		std::jthread m_worker; // Joins before any state above is destroyed.
	};
}
