#include "API/Health.h"

#include <map>
#include <mutex>
#include <set>
#include <vector>

#include <nlohmann/json.hpp>

#include "API/OSFSettings_Diagnostics.h"
#include "Registry/ContentRegistry.h"
#include "Registry/SoundRegistry.h"

#include "Util/DiagnosticText.h"
namespace OSF::API::Health
{
	namespace
	{
		using json = nlohmann::json;

		constexpr const char* kModId = "osf.animation";

		// Pre-Connect reports (the game-version check runs at plugin load) wait here.
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
			json                     context;
		};

		std::mutex         g_lock;
		OSFSettings::API::Diagnostics::Client g_client;  // static lifetime: the bridge outlives us
		bool               g_connectAttempted = false;
		std::vector<Op>    g_buffered;

		std::mutex            g_registryLock;
		std::set<std::string> g_registryIssues;

		// Caller holds g_lock and has a connected diagnostics client.
		void ApplyLocked(const Op& a_op)
		{
			using namespace OSFSettings::API;
			auto status = Status::Ok;
			if (a_op.kind == Op::Kind::kClear) {
				status = g_client.Clear(kModId, a_op.id.c_str());
			} else {
				const char* title = "Animation content could not be loaded";
				const char* next = "Check OSF Animation.log and repair or reinstall the affected content pack.";
				if (a_op.code == "boot.untested-game-version") {
					title = "This Starfield version has not been verified";
					next = "Check for an OSF Animation update supporting your game version.";
				} else if (a_op.code == "compat.needs-newer-osf-animation") {
					title = "A mod requires a newer OSF Animation";
					next = "Update OSF Animation to the version required by this mod.";
				} else if (a_op.code == "fade.disabled") {
					title = "Scene screen fades are unavailable";
					next = "Update OSF Animation for this Starfield version.";
				} else if (a_op.code == "sound.pack-load") {
					title = "Animation sounds could not be loaded";
				} else if (a_op.code == "alignment.storage") {
					title = "Scene alignment overrides could not be loaded or saved";
					next = "Check alignment-overrides.json in the SFSE OSF user folder and its write permissions. Scenes can still play.";
				}
				std::string impact = Util::SanitizeDiagnosticPaths(a_op.subject);
				if (a_op.context.is_object()) {
					for (const auto& [key, value] : a_op.context.items()) {
						if (!impact.empty()) impact += "\n";
						impact += key + ": " + Util::SanitizeDiagnosticPaths(value.is_string() ? value.get<std::string>() : value.dump());
					}
				}
				status = g_client.Report({
					.modId = kModId, .id = a_op.id.c_str(),
					.severity = a_op.severity == Severity::kError ? Diagnostics::Severity::Error : Diagnostics::Severity::Warning,
					.title = title, .impact = impact.c_str(), .nextSteps = next
				});
			}
			if (status != Status::Ok) REX::WARN("[Config] Could not update Mod Issue '{}' (status {})", a_op.id, static_cast<unsigned>(status));
		}

		void Submit(Op&& a_op)
		{
			std::lock_guard lock(g_lock);
			if (g_client) {
				ApplyLocked(a_op);
				return;
			}
			if (g_connectAttempted || g_buffered.size() >= kMaxBuffered) {
				return;  // provider absent or buffer full; log only
			}
			g_buffered.push_back(std::move(a_op));
		}

		// "[error] '<file>': ..." -> "'<file>': ..."; the card already carries severity.
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
			if (g_connectAttempted) {
				return;
			}
			g_connectAttempted = true;
			if (!g_client.Init()) {
				g_buffered.clear();
				g_buffered.shrink_to_fit();
				return;
			}
			replay.swap(g_buffered);
			for (const auto& op : replay) {
				ApplyLocked(op);
			}
		}
		if (!replay.empty()) {
			REX::INFO("[Feature] Mod Issues connected — {} buffered report(s) flushed", replay.size());
		} else {
			REX::INFO("[Feature] Mod Issues connected");
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
			.context = a_context && a_context->is_object() ? *a_context : json{},
		});
	}

	void Clear(std::string_view a_id)
	{
		Submit(Op{ .kind = Op::Kind::kClear, .id = std::string(a_id) });
	}

	void ReportRegistryLoad()
	{
		std::lock_guard registryLock(g_registryLock);

		// One card per file, not per problem.
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

		// Host caps context at 8 entries / 240 chars per value: show the first few plus a count.
		constexpr std::size_t kShownProblems = 4;
		const auto            buildContext = [](const std::vector<std::string>& a_lines, const std::string& a_file) {
			json context = json::object();
			if (!a_file.empty()) {
				context["file"] = a_file;  // bare filename: no user paths
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
