#include "API/Health.h"

#include <map>
#include <mutex>
#include <set>

#include <nlohmann/json.hpp>

#include "API/OSFUI_API.h"
#include "Registry/SceneRegistry.h"
#include "Registry/SoundRegistry.h"

namespace OSF::API::Health
{
	namespace
	{
		using json = nlohmann::json;

		// Our source in the pane. The host validates it and assigns it to every
		// report we make — it is not something a payload can claim — and it
		// namespaces our ids and codes, so nothing here can collide with another
		// mod's report or with one of OSF UI's own.
		constexpr const char* kModId = "osf.animation";

		// Reports made before Connect() (the game-version check runs at plugin
		// load, well before the bridge exists) wait here. Bounded because a
		// buffer that only ever grows is a leak with good intentions; in
		// practice the pre-Connect window produces one or two entries.
		constexpr std::size_t kMaxBuffered = 32;

		struct Op
		{
			enum class Kind
			{
				kReport,
				kClear,
				kKeepOnly,
			};
			Kind                     kind{ Kind::kReport };
			std::string              id;
			std::string              code;
			Severity                 severity{ Severity::kWarning };
			std::string              subject;
			std::string              contextJson;  // "" when there is none
			std::vector<std::string> keep;
		};

		std::mutex         g_lock;
		OSFUI::API::Client g_client;  // static lifetime: the bridge outlives us
		bool               g_connected = false;
		std::vector<Op>    g_buffered;

		// Every id we currently have raised. The host's sweep (ClearIssuesExcept)
		// is scoped to the whole MOD, not to one producer's ids, so a producer
		// that reconciles its own set has to hand over the complete live list or
		// it silently withdraws every OTHER producer's card. Tracking here is
		// what lets ReportRegistryLoad reconcile packs without clearing the
		// game-version or wheel-pins conditions.
		std::set<std::string> g_live;

		// Caller must hold g_lock and have checked g_connected.
		void ApplyLocked(const Op& a_op)
		{
			switch (a_op.kind) {
			case Op::Kind::kReport:
				g_client.ReportIssue(kModId, a_op.id.c_str(), a_op.code.c_str(),
					a_op.severity == Severity::kError ?
						OSFUI::API::IssueSeverity::kError :
						OSFUI::API::IssueSeverity::kWarning,
					a_op.subject.c_str(),
					a_op.contextJson.empty() ? nullptr : a_op.contextJson.c_str());
				break;
			case Op::Kind::kClear:
				g_client.ClearIssue(kModId, a_op.id.c_str());
				break;
			case Op::Kind::kKeepOnly:
				{
					json ids = json::array();
					for (const auto& id : a_op.keep) {
						ids.push_back(id);
					}
					g_client.ClearIssuesExcept(kModId, ids.dump().c_str());
				}
				break;
			}
		}

		// Caller must hold g_lock. Mirrors what the registry will hold after
		// a_op is applied, so it stays true whether the op is applied now or
		// replayed from the buffer later.
		void TrackLocked(const Op& a_op)
		{
			switch (a_op.kind) {
			case Op::Kind::kReport:
				g_live.insert(a_op.id);
				break;
			case Op::Kind::kClear:
				g_live.erase(a_op.id);
				break;
			case Op::Kind::kKeepOnly:
				{
					const std::set<std::string> keep(a_op.keep.begin(), a_op.keep.end());
					std::erase_if(g_live, [&](const std::string& a_id) { return !keep.contains(a_id); });
				}
				break;
			}
		}

		void Submit(Op&& a_op)
		{
			std::lock_guard lock(g_lock);
			TrackLocked(a_op);
			if (g_connected) {
				ApplyLocked(a_op);
				return;
			}
			if (g_buffered.size() >= kMaxBuffered) {
				return;  // pre-bridge flood; the log lines still carry every one
			}
			g_buffered.push_back(std::move(a_op));
		}

		// ---- registry load problems -----------------------------------------

		// Both registries record their problems as prose lines, "[error] ..." /
		// "[warn] ...", for OSF.GetSceneLoadErrors and the browser's refusal
		// hints. A card wants the FILE, so that a player can find and fix (or
		// remove) it — and a line names its file in different positions
		// depending on which check failed ("'<file>' missing 'schema'", but
		// "duplicate scene id 'x' in '<file>'"). The one thing every shape has
		// in common is that the filename is the quoted token ending in .json,
		// so that is what we look for rather than "the first quoted token".
		// Anchor on ".json" and grow outwards to the surrounding quotes rather
		// than walking quote pairs left to right: a filename may itself contain
		// an apostrophe ("bob's pack.osf.json" is a legal Windows name), which
		// makes pairing quotes ambiguous. An opening quote is one that starts
		// the line or follows a space; the closing quote is the first after the
		// extension. Anything else (no ".json", no quotes) yields "" and the
		// line joins the cross-file card instead of being dropped.
		std::string FileOf(std::string_view a_line)
		{
			const auto dot = a_line.find(".json");
			if (dot == std::string_view::npos) {
				return {};
			}
			const auto close = a_line.find('\'', dot);
			if (close == std::string_view::npos) {
				return {};
			}
			std::size_t open = std::string_view::npos;
			for (std::size_t i = dot; i-- > 0;) {
				if (a_line[i] == '\'' && (i == 0 || a_line[i - 1] == ' ')) {
					open = i;
					break;
				}
			}
			if (open == std::string_view::npos) {
				return {};
			}
			return std::string(a_line.substr(open + 1, close - open - 1));
		}

		bool IsError(std::string_view a_line)
		{
			return a_line.starts_with("[error]");
		}

		// "[error] '<file>': ..." -> "'<file>': ..." — the severity is already
		// carried by the card, three ways.
		std::string StripPrefix(const std::string& a_line)
		{
			const auto close = a_line.find(']');
			if (close == std::string::npos || close + 2 >= a_line.size()) {
				return a_line;
			}
			return a_line.substr(close + 2);
		}

		struct FileProblems
		{
			bool                     error{ false };
			std::vector<std::string> lines;
		};

		// The ids ReportRegistryLoad owns, and therefore the only ones its sweep
		// may retire.
		bool IsPackIssue(std::string_view a_id)
		{
			return a_id.starts_with("pack:") || a_id == "packs:cross-file";
		}

		// Everything currently raised that is NOT ours to reconcile — the other
		// producers' cards, which have to survive the sweep.
		std::vector<std::string> LiveNonPackIssues()
		{
			std::lock_guard          lock(g_lock);
			std::vector<std::string> out;
			for (const auto& id : g_live) {
				if (!IsPackIssue(id)) {
					out.push_back(id);
				}
			}
			return out;
		}
	}

	void Connect()
	{
		std::vector<Op> replay;
		{
			std::lock_guard lock(g_lock);
			if (g_connected) {
				return;
			}
			// A missing OSF UI, a MAJOR mismatch, or a host older than 1.7 all
			// land here as "no". None of them is an error: reporting is a
			// courtesy to the player, never a dependency of playback.
			if (!g_client.Init() || !g_client.Has(OSFUI::API::Feature::kDiagnostics)) {
				g_buffered.clear();
				g_buffered.shrink_to_fit();
				return;
			}
			g_connected = true;
			replay.swap(g_buffered);
			g_buffered.shrink_to_fit();
			for (const auto& op : replay) {
				ApplyLocked(op);
			}
		}
		if (!replay.empty()) {
			REX::INFO("[Health] OSF UI System Health connected — {} buffered report(s) flushed", replay.size());
		} else {
			REX::INFO("[Health] OSF UI System Health connected");
		}
	}

	void Report(std::string_view a_id, std::string_view a_code, Severity a_severity,
		std::string_view a_subject, const json* a_context)
	{
		Submit(Op{
			.kind = Op::Kind::kReport,
			.id = std::string(a_id),
			.code = std::string(a_code),
			.severity = a_severity,
			.subject = std::string(a_subject),
			.contextJson = a_context && a_context->is_object() ? a_context->dump() : std::string{},
		});
	}

	void Clear(std::string_view a_id)
	{
		Submit(Op{ .kind = Op::Kind::kClear, .id = std::string(a_id) });
	}

	void KeepOnly(const std::vector<std::string>& a_ids)
	{
		Submit(Op{ .kind = Op::Kind::kKeepOnly, .keep = a_ids });
	}

	void ReportRegistryLoad()
	{
		// One card per FILE, not per problem: twelve rejected scenes in one bad
		// pack are one thing to fix, and twelve cards would bury every other
		// condition in the pane.
		std::map<std::string, FileProblems> byFile;
		std::vector<std::string>            unattributed;
		bool                                unattributedError = false;

		const auto collect = [&](const std::vector<std::string>& a_lines) {
			for (const auto& line : a_lines) {
				const auto file = FileOf(line);
				if (file.empty()) {
					// A cross-file problem (a dangling `use` between packs, say):
					// no single file to name, so it goes to one shared card
					// rather than being dropped.
					unattributedError = unattributedError || IsError(line);
					unattributed.push_back(StripPrefix(line));
					continue;
				}
				auto& rec = byFile[file];
				rec.error = rec.error || IsError(line);
				rec.lines.push_back(StripPrefix(line));
			}
		};
		collect(Registry::SceneRegistry::GetSingleton().LoadErrors());
		collect(Registry::SoundRegistry::GetSingleton().LoadErrors());

		// `context` is capped at 8 entries and 240 chars per value by the host,
		// so the card shows the first few problems and says how many there were.
		// Someone who needs all of them has the log and OSF.GetSceneLoadErrors.
		constexpr std::size_t kShownProblems = 4;
		const auto            buildContext = [](const std::vector<std::string>& a_lines, const std::string& a_file) {
            json context = json::object();
            if (!a_file.empty()) {
                context["file"] = a_file;  // bare filename: an absolute path would name the player's machine
            }
            context["problems"] = a_lines.size();
            for (std::size_t i = 0; i < a_lines.size() && i < kShownProblems; ++i) {
                context["problem" + std::to_string(i + 1)] = a_lines[i];
            }
            return context;
		};

		std::vector<std::string> live;
		for (const auto& [file, problems] : byFile) {
			// Sound packs and scene packs fail for different reasons and are
			// fixed in different places, so they get different codes even though
			// they arrive through the same sweep.
			const std::string code = file.ends_with(".sounds.json") ? "sound.pack-load" : "catalog.pack-load";
			const std::string id = "pack:" + file;
			const auto        context = buildContext(problems.lines, file);
			Report(id, code, problems.error ? Severity::kError : Severity::kWarning, file, &context);
			live.push_back(id);
		}
		if (!unattributed.empty()) {
			const auto context = buildContext(unattributed, {});
			Report("packs:cross-file", "catalog.cross-file", unattributedError ? Severity::kError : Severity::kWarning,
				"", &context);
			live.push_back("packs:cross-file");
		}

		// The reconcile that makes a reload worth doing: a pack the player has
		// since fixed loses its card (to "Resolved this session"), and one that
		// is still broken keeps its identity and its occurrence count. The keep
		// list carries every OTHER producer's live id too — the host's sweep is
		// mod-wide, so anything omitted here would be withdrawn as collateral.
		auto keep = LiveNonPackIssues();
		keep.insert(keep.end(), live.begin(), live.end());
		KeepOnly(keep);
	}
}
