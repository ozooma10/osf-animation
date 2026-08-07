#include "API/Health.h"

#include <map>
#include <mutex>
#include <set>

#include <nlohmann/json.hpp>

#include "API/OSFUI_API.h"
#include "Registry/SceneRegistry.h"
#include "Registry/SoundRegistry.h"

#include "Util/DiagnosticText.h"
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
			};
			Kind                     kind{ Kind::kReport };
			std::string              id;
			std::string              code;
			Severity                 severity{ Severity::kWarning };
			std::string              subject;
			std::string              contextJson;  // "" when there is none
		};

		std::mutex         g_lock;
		OSFUI::API::Client g_client;  // static lifetime: the bridge outlives us
		bool               g_connected = false;
		std::vector<Op>    g_buffered;

		std::mutex            g_registryLock;
		std::set<std::string> g_registryIssues;

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
			}
		}

		void Submit(Op&& a_op)
		{
			std::lock_guard lock(g_lock);
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

		// Registry snapshots own file/severity attribution; Health only removes the
		// redundant prose severity prefix before forwarding their problem text.
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
			std::string              code;
			std::string              subject;
			bool                     error{ false };
			std::vector<std::string> lines;
		};

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
			REX::INFO("[UI] System Health connected — {} buffered report(s) flushed", replay.size());
		} else {
			REX::INFO("[UI] System Health connected");
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

	void ReportRegistryLoad()
	{
		std::lock_guard registryLock(g_registryLock);

		// One card per file, not per problem: one broken pack should produce one
		// actionable condition regardless of how many entries it rejected.
		std::map<std::string, FileProblems> byId;
		for (const auto& stats : Registry::ContentRegistry::GetSingleton().FileStats()) {
			if (stats.problems.empty()) {
				continue;
			}
			const std::string id = stats.path.empty() ? "packs:cross-file" : "pack:" + stats.path;
			auto& rec = byId[id];
			rec.code = stats.path.empty() ? "catalog.cross-file" : "catalog.pack-load";
			rec.subject = stats.file;
			rec.error = rec.error || stats.errors > 0;
			for (const auto& line : stats.problems) {
				rec.lines.push_back(StripPrefix(line.message));
			}
		}
		for (const auto& stats : Registry::SoundRegistry::GetSingleton().FileStats()) {
			if (stats.problems.empty()) {
				continue;
			}
			const std::string id = stats.path.empty() ? "packs:cross-file" : "pack:" + stats.path;
			auto& rec = byId[id];
			if (rec.code.empty()) {
				rec.code = stats.file.empty() ? "catalog.cross-file" : "sound.pack-load";
			}
			rec.subject = stats.file;
			rec.error = rec.error || stats.errors > 0;
			for (const auto& line : stats.problems) {
				rec.lines.push_back(StripPrefix(line));
			}
		}

		// `context` is capped at 8 entries and 240 chars per value by the host,
		// so the card shows the first few problems and says how many there were.
		// Someone who needs all of them has the log and OSFAdvanced.GetSceneLoadErrors.
		constexpr std::size_t kShownProblems = 4;
		const auto            buildContext = [](const std::vector<std::string>& a_lines, const std::string& a_file) {
			json context = json::object();
			if (!a_file.empty()) {
				context["file"] = a_file;  // bare filename: an absolute path would name the player's machine
			}
			context["problems"] = a_lines.size();
			for (std::size_t i = 0; i < a_lines.size() && i < kShownProblems; ++i) {
				context["problem" + std::to_string(i + 1)] = Util::SanitizeDiagnosticPaths(a_lines[i]);
			}
			return context;
		};

		std::set<std::string> live;
		for (const auto& [id, problems] : byId) {
			const auto context = buildContext(problems.lines, problems.subject);
			Report(id, problems.code, problems.error ? Severity::kError : Severity::kWarning,
				problems.subject, &context);
			live.insert(id);
		}

		for (const auto& id : g_registryIssues) {
			if (!live.contains(id)) {
				Clear(id);
			}
		}
		g_registryIssues = std::move(live);
	}
}
