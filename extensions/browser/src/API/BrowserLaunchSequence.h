#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace OSF::API
{
	// Stop accepts a request; ownership may survive several frame drains while
	// preparation rolls back. Only start once the whole cast has been released.
	class BrowserLaunchSequence : public std::enable_shared_from_this<BrowserLaunchSequence>
	{
	public:
		using Clock = std::chrono::steady_clock;
		class Requests
		{
			friend class BrowserLaunchSequence;
			std::vector<std::weak_ptr<BrowserLaunchSequence>> pending;
		};
		struct Hooks
		{
			std::function<std::vector<std::int32_t>()> owners;
			std::function<bool(std::int32_t)> stop;
			std::function<void()> start;
			std::function<void(const char*)> fail;
			// The browser session queue cancels this callback on hide/world reset.
			std::function<void(std::function<void()>)> post;
			std::function<Clock::time_point()> now = [] { return Clock::now(); };
		};

		static void Begin(Requests& a_requests, std::vector<std::uint32_t> a_actors, Hooks a_hooks)
		{
			auto pending = std::shared_ptr<BrowserLaunchSequence>(new BrowserLaunchSequence(std::move(a_actors), std::move(a_hooks)));
			std::vector<std::int32_t> acceptedStops;
			std::erase_if(a_requests.pending, [&](const auto& weak) {
				const auto prior = weak.lock();
				if (!prior || prior->m_finished) return true;
				acceptedStops.insert(acceptedStops.end(), prior->m_stopping.begin(), prior->m_stopping.end());
				if (std::any_of(prior->m_actors.begin(), prior->m_actors.end(), [&](auto actor) {
					return std::find(pending->m_actors.begin(), pending->m_actors.end(), actor) != pending->m_actors.end();
				})) {
					prior->m_finished = true; // A superseded request never sends a stale reply.
					return true;
				}
				return false;
			});
			a_requests.pending.emplace_back(pending);
			for (const auto owner : pending->m_hooks.owners()) {
				if (owner == 0 || std::find(pending->m_stopping.begin(), pending->m_stopping.end(), owner) != pending->m_stopping.end()) continue;
				// Another pending request may already be stopping this scene, including
				// a disjoint cast selected from that same scene. Never repeat its stop.
				if (std::find(acceptedStops.begin(), acceptedStops.end(), owner) == acceptedStops.end() && !pending->m_hooks.stop(owner)) {
					pending->Fail("The current scene could not be stopped — try again after it ends");
					return;
				}
				pending->m_stopping.push_back(owner);
			}
			pending->Poll();
		}

	private:
		BrowserLaunchSequence(std::vector<std::uint32_t> a_actors, Hooks a_hooks) :
			m_hooks(std::move(a_hooks)), m_started(m_hooks.now()), m_actors(std::move(a_actors)) {}
		void Fail(const char* a_reason)
		{
			if (m_finished) return;
			m_finished = true;
			m_hooks.fail(a_reason);
		}
		void Poll()
		{
			if (m_finished) return;
			bool busy = false;
			for (const auto owner : m_hooks.owners()) {
				if (owner == 0) continue;
				if (std::find(m_stopping.begin(), m_stopping.end(), owner) == m_stopping.end()) {
					Fail("A selected cast member joined another scene — try again");
					return;
				}
				busy = true;
			}
			if (!busy) {
				m_finished = true;
				m_hooks.start();
			} else if (m_hooks.now() - m_started >= std::chrono::seconds(5)) {
				Fail("The current scene is still releasing its cast — try again after it ends");
			} else {
				m_hooks.post([self = shared_from_this()] { self->Poll(); });
			}
		}

		Hooks m_hooks;
		Clock::time_point m_started;
		std::vector<std::uint32_t> m_actors;
		std::vector<std::int32_t> m_stopping;
		bool m_finished{};
	};
}
