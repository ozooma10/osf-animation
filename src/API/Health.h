#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

// OSF Animation's producer side of OSF UI's System Health pane (native bridge
// ABI 1.7). One shared pane for the whole game — a player who has to know which
// mod noticed a problem before they know where to look has it backwards — so
// this mod reports into OSF UI's registry rather than growing a diagnostics
// screen of its own. See docs/HEALTH_REPORTING.md.
//
// WHAT BELONGS HERE. Only conditions that are durable (still true when the
// player reads the card), actionable (they can do something, even if that
// something is telling the author), and worth interrupting them for. A failed
// single OSF.Play, a one-frame hiccup, a count of loaded packs: all log lines,
// none of them cards. The pane's whole promise is that everything on it is true
// right now, and a pane full of noise is a pane players learn to skip.
//
// Every report also keeps its existing REX log line. The card is additive: the
// log stays the developer's record, the card is the player's.
//
// THREADING: every entry point is safe from any thread. Reports made before the
// bridge exists (the game-version check runs at plugin load) are buffered and
// replayed by Connect().
namespace OSF::API::Health
{
	enum class Severity
	{
		kWarning,  // degraded, still usable
		kError,    // this part does not work
	};

	// Fetch the OSF UI bridge and flush anything buffered. Call once, on the
	// game thread, before the loaders run — it is separate from InstallUIBridge
	// because health reporting must survive a host too old for everything else,
	// and because the loaders report before the browser view is installed.
	// A missing or pre-1.7 OSF UI is a normal outcome: every call below then
	// degrades to a no-op and the log lines carry on alone.
	void Connect();

	// Raise or refresh one condition. `a_id` is our dedupe key — reporting a
	// live id again bumps its occurrence count in place rather than stacking a
	// card. `a_code` is the stable machine code for the KIND of condition; it is
	// never player-facing prose, because OSF UI owns the wording (localizable,
	// and not authorable by a mod). `a_context` must be a flat object of
	// string/number/bool values, bounded and path-sanitized by the host — pass
	// bare filenames, never absolute paths.
	void Report(std::string_view a_id, std::string_view a_code, Severity a_severity,
		std::string_view a_subject = "", const nlohmann::json* a_context = nullptr);

	// Withdraw one condition; it moves to the pane's "Resolved this session"
	// list, which is what a player wants to see after a retry. Cheap and safe to
	// call unconditionally — clearing something that was never raised does
	// nothing.
	void Clear(std::string_view a_id);

	// Report the scene + sound registries' load problems as one card per failed
	// file, then sweep the files that are now clean. Call after LoadAll() —
	// including the OSF.ReloadScenes path, which is exactly where the sweep
	// earns its keep: fixing a pack and reloading moves its card to history
	// instead of leaving a stale one behind.
	void ReportRegistryLoad();
}
