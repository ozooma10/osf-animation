#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace OSF::Preparation
{
    using Token = std::uint64_t;
    enum class Kind : std::uint32_t { Start, Handoff, Node, Stage };
    enum class Phase : std::uint32_t { Plan, Apply, Ready, Validate };
    enum class Decision : std::uint32_t { Approve, Pending, Veto };
    enum class Outcome : std::uint32_t { Committed, Vetoed, Failed, Cancelled, TimedOut, OwnerWithdrawn, WorldReset };
    struct Context {
        Token attempt{};
        std::int32_t scene{};
        Kind kind{};
        std::string fromScene, fromNode, toScene, toNode;
        std::int32_t fromStage{-1}, toStage{-1};
        std::vector<std::uint32_t> actors;
    };
    struct Provider {
        std::int32_t order{};
        std::uint32_t timeoutMs{5000};
        std::function<Decision(const Context&, Phase)> invoke;
        std::function<void(const Context&, Outcome)> finish;
    };
    struct CommitGuard {
        std::function<bool()> acquire;
        std::function<void()> release; // paired even when acquire refuses or throws
    };
    struct Hooks {
        std::function<bool()> valid; // identity/generation check after every external callback
        std::function<bool()> preflight; // all collected plans, before any mutation
        std::function<bool()> apply;
        std::function<Decision()> ready;
        std::function<bool()> commit;
        std::function<void(Outcome, bool)> finish; // bool: application began, so rollback may be needed
		std::function<Outcome()>           invalidReason;  // optional: distinguish world reset from ordinary cancellation
        std::function<bool()> beforeCommit; // final reconciliation, before guard acquisition
        std::function<bool()> checkCommit; // memory-only check after every guard acquired
	};

    // Serial frame-drain coordinator. Callbacks run without internal locks and may cancel,
    // reset, unregister, or enqueue other attempts. Tick never recursively pumps new work.
    class Coordinator {
    public:
        Token Register(Provider provider);
        bool Unregister(Token provider);
        Token Begin(Context context, Hooks hooks, std::uint64_t nowMs);
        void Tick(std::uint64_t nowMs);
        bool Cancel(Token attempt, Outcome reason = Outcome::Cancelled);
        void Reset();
        bool Guard(Token attempt, Token provider, CommitGuard guard);
        bool Contains(Token attempt) const { return _attempts.contains(attempt); }
        std::size_t PendingCount() const { return _attempts.size(); }
		std::optional<Token> CurrentProvider(Token attempt, std::optional<Phase> phase = {}) const;
		std::vector<Token>   Providers(Token attempt) const;

	private:
        struct Entry { Token token; Provider provider; };
        struct Attempt {
            Context context;
            Hooks hooks;
            std::vector<Entry> providers;
            std::vector<bool> entered;
            Phase phase{Phase::Plan};
            std::size_t index{};
            std::uint64_t deadline{};
            std::optional<Outcome> cancelled;
            bool applied{}, pumping{}, finishing{};
            Token calling{};
            struct GuardEntry { Token provider; CommitGuard guard; bool entered{}; };
            std::vector<GuardEntry> guards;
            bool sealing{};
        };
        Token Next();
        bool Valid(const std::shared_ptr<Attempt>&);
        void Pump(const std::shared_ptr<Attempt>&, std::uint64_t nowMs);
        void Finish(const std::shared_ptr<Attempt>&, Outcome);
        Token _next{1};
        bool _ticking{}, _resetting{};
        std::map<Token, Provider> _providers;
        std::map<Token, std::shared_ptr<Attempt>> _attempts;
    };
}
