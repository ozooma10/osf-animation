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

		static void Begin(Hooks a_hooks)
		{
			auto pending = std::shared_ptr<BrowserLaunchSequence>(new BrowserLaunchSequence(std::move(a_hooks)));
			for (const auto owner : pending->m_hooks.owners()) {
				if (owner != 0 && std::find(pending->m_stopping.begin(), pending->m_stopping.end(), owner) == pending->m_stopping.end()) {
					pending->m_stopping.push_back(owner);
				}
			}
			for (const auto owner : pending->m_stopping) {
				if (!pending->m_hooks.stop(owner)) {
					pending->m_hooks.fail("The current scene could not be stopped — try again after it ends");
					return;
				}
			}
			pending->Poll();
		}

	private:
		explicit BrowserLaunchSequence(Hooks a_hooks) : m_hooks(std::move(a_hooks)), m_started(m_hooks.now()) {}
		void Poll()
		{
			bool busy = false;
			for (const auto owner : m_hooks.owners()) {
				if (owner == 0) continue;
				if (std::find(m_stopping.begin(), m_stopping.end(), owner) == m_stopping.end()) {
					m_hooks.fail("A selected cast member joined another scene — try again");
					return;
				}
				busy = true;
			}
			if (!busy) {
				m_hooks.start();
			} else if (m_hooks.now() - m_started >= std::chrono::seconds(5)) {
				m_hooks.fail("The current scene is still releasing its cast — try again after it ends");
			} else {
				m_hooks.post([self = shared_from_this()] { self->Poll(); });
			}
		}

		Hooks m_hooks;
		Clock::time_point m_started;
		std::vector<std::int32_t> m_stopping;
	};
}
