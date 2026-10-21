#include "Preparation/Coordinator.h"

#include <algorithm>
#include <limits>

namespace OSF::Preparation
{
    Token Coordinator::Next() { return _next && _next != std::numeric_limits<Token>::max() ? _next++ : 0; }
    Token Coordinator::Register(Provider provider)
    {
		if (!provider.invoke || !provider.finish || !provider.timeoutMs || provider.timeoutMs > 5000)
			return 0;
		const auto id = Next();
        if (id) _providers.emplace(id, std::move(provider));
        return id;
    }
    bool Coordinator::Unregister(Token provider)
    {
        if (!_providers.erase(provider)) return false;
        std::vector<Token> cancel;
        for (const auto& [id, a] : _attempts)
            if (std::any_of(a->providers.begin(), a->providers.end(), [&](const auto& e) { return e.token == provider; }))
                cancel.push_back(id);
        for (auto id : cancel) Cancel(id, Outcome::OwnerWithdrawn);
        return true;
    }
    Token Coordinator::Begin(Context context, Hooks hooks, std::uint64_t nowMs)
    {
        if (_resetting || !context.scene || context.actors.empty() || !hooks.valid || !hooks.preflight ||
            !hooks.apply || !hooks.ready || !hooks.commit || !hooks.finish) return 0;
        if (std::any_of(_attempts.begin(), _attempts.end(), [&](const auto& e) { return e.second->context.scene == context.scene; })) return 0;
        const auto id = Next();
        if (!id) return 0;
        auto a = std::make_shared<Attempt>();
        context.attempt = id;
        a->context = std::move(context); a->hooks = std::move(hooks);
        std::uint32_t timeout = 5000;
        for (const auto& [token, provider] : _providers) {
            a->providers.push_back({token, provider});
            timeout = std::min(timeout, provider.timeoutMs);
        }
        std::stable_sort(a->providers.begin(), a->providers.end(), [](const auto& x, const auto& y) {
            return x.provider.order < y.provider.order;
        });
        a->entered.resize(a->providers.size());
        a->deadline = nowMs + timeout;
        _attempts.emplace(id, a);
        return id;
    }
    bool Coordinator::Valid(const std::shared_ptr<Attempt>& a)
    {
        if (a->cancelled || !Contains(a->context.attempt)) return false;
		if (!a->hooks.valid()) {
			a->cancelled = a->hooks.invalidReason ? a->hooks.invalidReason() : Outcome::Cancelled;
			return false;
		}
		return !a->cancelled && Contains(a->context.attempt);
    }
	std::optional<Token> Coordinator::CurrentProvider(Token attempt, std::optional<Phase> phase) const
	{
        const auto it = _attempts.find(attempt);
		if (it == _attempts.end() || (phase && it->second->phase != *phase) || !it->second->calling || it->second->cancelled)
			return {};
		return it->second->calling;
    }
	std::vector<Token> Coordinator::Providers(Token attempt) const
	{
		std::vector<Token> out;
		const auto         it = _attempts.find(attempt);
		if (it != _attempts.end())
			for (const auto& e : it->second->providers) out.push_back(e.token);
		return out;
	}
	void Coordinator::Finish(const std::shared_ptr<Attempt>& a, Outcome outcome)
    {
        if (a->finishing) return;
        a->finishing = true;
        _attempts.erase(a->context.attempt); // callbacks cannot cancel/re-publish this attempt
        for (std::size_t i = a->providers.size(); i-- > 0;) {
            if (!a->entered[i]) continue;
			if (!a->hooks.valid())
				outcome = a->hooks.invalidReason ? a->hooks.invalidReason() : Outcome::Cancelled;
			try { a->providers[i].provider.finish(a->context, outcome); } catch (...) {}
        }
		try {
			if (!a->hooks.valid())
				outcome = a->hooks.invalidReason ? a->hooks.invalidReason() : Outcome::Cancelled;
			a->hooks.finish(outcome, a->applied);
		} catch (...) {}
	}
    bool Coordinator::Cancel(Token attempt, Outcome reason)
    {
        const auto it = _attempts.find(attempt);
        if (it == _attempts.end()) return false;
        const auto a = it->second;
        a->cancelled = reason;
        if (!a->pumping) Finish(a, reason);
        return true;
    }
    void Coordinator::Reset()
    {
        if (_resetting) return;
        _resetting = true;
        std::vector<Token> ids;
        for (const auto& [id, a] : _attempts) ids.push_back(id);
        for (auto id : ids) Cancel(id, Outcome::WorldReset);
        _resetting = false;
        // Registrations survive; attempt tokens never recycle.
    }
    void Coordinator::Pump(const std::shared_ptr<Attempt>& a, std::uint64_t nowMs)
    {
        a->pumping = true;
        try {
            if (nowMs >= a->deadline) a->cancelled = Outcome::TimedOut;
            while (Valid(a)) {
                if (a->index < a->providers.size()) {
                    const auto i = a->index;
                    a->entered[i] = true; a->calling = a->providers[i].token;
                    const auto decision = a->providers[i].provider.invoke(a->context, a->phase);
                    a->calling = 0;
                    if (!Valid(a)) break;
                    if (decision == Decision::Pending && (a->phase == Phase::Plan || a->phase == Phase::Ready)) break;
                    if (decision != Decision::Approve) { a->cancelled = a->applied ? Outcome::Failed : Outcome::Vetoed; break; }
                    ++a->index;
                    continue;
                }
                if (a->phase == Phase::Plan) {
                    const bool accepted = a->hooks.preflight();
                    if (!Valid(a)) break;
                    if (!accepted) { a->cancelled = Outcome::Vetoed; break; }
                    a->applied = true;
                    const bool applied = a->hooks.apply();
                    if (!Valid(a)) break;
                    if (!applied) { a->cancelled = Outcome::Failed; break; }
                    a->phase = Phase::Apply; a->index = 0;
                } else if (a->phase == Phase::Apply) {
                    a->phase = Phase::Ready; a->index = 0;
                } else if (a->phase == Phase::Ready) {
                    const auto ready = a->hooks.ready();
                    if (!Valid(a)) break;
                    if (ready == Decision::Pending) break;
                    if (ready != Decision::Approve) { a->cancelled = Outcome::Failed; break; }
                    a->phase = Phase::Validate; a->index = 0;
                } else {
                    const bool committed = a->hooks.commit();
                    if (!Valid(a)) break;
                    if (!committed) { a->cancelled = Outcome::Failed; break; }
                    Finish(a, Outcome::Committed);
                    break;
                }
            }
        } catch (...) { a->cancelled = Outcome::Failed; }
        a->calling = 0; a->pumping = false;
        if (Contains(a->context.attempt) && a->cancelled) Finish(a, *a->cancelled);
    }
    void Coordinator::Tick(std::uint64_t nowMs)
    {
        if (_ticking) return;
        _ticking = true;
		struct Guard
		{
			bool& flag;
			~Guard() { flag = false; }
		} guard{ _ticking };
		std::vector<std::shared_ptr<Attempt>> batch;
        for (const auto& [id, a] : _attempts) batch.push_back(a);
        for (const auto& a : batch) if (Contains(a->context.attempt)) Pump(a, nowMs);
    }
}
