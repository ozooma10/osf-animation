#include "API/EngineClient.h"
#include "API/UIBridgeCatalog.h"
#include "API/UIKeywordLabel.h"

#include "Serialization/WheelPins.h"
#include "Util/StringUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

namespace OSF::API::UIBridgeCatalog
{
	using json = nlohmann::json;

	namespace
	{
		// Loops an open-ended hold stage is assumed to run, for time estimates.
		constexpr float kHoldLoopEstimate = 2.0f;
		bool IsEmote(const Catalog::Node& a_def)
		{
			return std::ranges::any_of(a_def.tags,
				[](const std::string& a_tag) { return Util::ToLower(a_tag).starts_with("player.emote."); });
		}

		bool IsWheelScene(const Catalog::Node& a_def)
		{
			return !a_def.library && !a_def.unlisted && a_def.roles.size() == 1 &&
			       !a_def.RequiresAnchor() && IsEmote(a_def);
		}

		const Catalog::Stage* WheelStage(const Catalog::Node& a_def, std::int32_t a_stage)
		{
			if (!a_def.library || a_def.roles.size() != 1 || a_def.RequiresAnchor() ||
			    (!a_def.species.empty() && a_def.species != "human") ||
			    a_stage < 0 || static_cast<std::size_t>(a_stage) >= a_def.stages.size()) {
				return nullptr;
			}
			const auto* stage = &a_def.stages[static_cast<std::size_t>(a_stage)];
			return stage->resolved && stage->clipCount > 0 ? stage : nullptr;
		}

		// Unknown durations serialize as null, never a sentinel the view could mistake for seconds.
		json SecOrNull(float a_sec)
		{
			return a_sec >= 0.0f ? json(a_sec) : json(nullptr);
		}

		struct RoleCard
		{
			std::string name;
			std::string gender;
		};
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(RoleCard, name, gender)

		struct StageCard
		{
			std::int32_t             index = 0;
			std::string              name;
			std::vector<std::string> tags;
			std::int32_t             clipCount = 0;
			std::int32_t             pinned = 0;  // 1-based wheel order
			// < 0 = unknown (clip not probed yet).
			float                    loopSec = -1.0f;
			float                    timerSec = 0.0f;   // 0 = none
			std::int32_t             loops = -1;        // -1 = once, 0 = hold, N = count
			bool                     openEnded = false;
			float                    estSec = -1.0f;
		};

		void to_json(json& a_out, const StageCard& a_stage)
		{
			a_out = {
				{ "index", a_stage.index },
				{ "name", a_stage.name },
				{ "tags", a_stage.tags },
				{ "clipCount", a_stage.clipCount },
				{ "pinned", a_stage.pinned },
				{ "loopSec", SecOrNull(a_stage.loopSec) },
				{ "timerSec", a_stage.timerSec > 0.0f ? json(a_stage.timerSec) : json(nullptr) },
				{ "loops", a_stage.loops >= 0 ? json(a_stage.loops) : json(nullptr) },
				{ "openEnded", a_stage.openEnded },
				{ "estSec", SecOrNull(a_stage.estSec) },
			};
		}

		struct Card
		{
			std::string              id;
			std::string              title;
			std::string              pack;
			std::string              folder;      // slash-delimited path within the pack
			std::string              sourceFile;  // file name only; never leak the install path
			std::string              sourcePath;  // Data/OSF-relative
			std::string              sourceKind;
			std::string              species;
			std::vector<std::string> tags;
			std::uint32_t            actorCount = 0;
			std::vector<RoleCard>    roles;
			std::int32_t             priority = 0;
			std::int32_t             weight = 1;
			bool                     hideApparel = true;
			bool                     playerInputLock = true;
			bool                     fade = false;
			bool                     requiresFurniture = false;
			std::string              worldPlacement = "anchorAndPin";
			std::vector<std::string> anchorNames;
			bool                     unlisted = false;
			bool                     wheelCustomized = false;  // whole-wheel state, mirrored onto every card
			std::int32_t             pinned = 0;  // 1-based wheel order; 0 = absent/default-derived
			std::vector<StageCard>   stages;  // empty for a non-linear graph
			float                    estSec = -1.0f;      // < 0 = none known
			bool                     estPartial = false;
			bool                     openEnded = false;
		};

		void to_json(json& a_out, const Card& a_card)
		{
			a_out = {
				{ "id", a_card.id },
				{ "title", a_card.title },
				{ "pack", a_card.pack },
				{ "folder", a_card.folder },
				{ "sourceFile", a_card.sourceFile },
				{ "species", a_card.species },
				{ "tags", a_card.tags },
				{ "sourcePath", a_card.sourcePath },
				{ "sourceKind", a_card.sourceKind },
				{ "actorCount", a_card.actorCount },
				{ "roles", a_card.roles },
				{ "priority", a_card.priority },
				{ "weight", a_card.weight },
				{ "hideApparel", a_card.hideApparel },
				{ "playerInputLock", a_card.playerInputLock },
				{ "fade", a_card.fade },
				{ "requiresFurniture", a_card.requiresFurniture },
				{ "placement", a_card.worldPlacement },
				{ "anchors", a_card.anchorNames },
				{ "unlisted", a_card.unlisted },
				{ "wheelCustomized", a_card.wheelCustomized },
				{ "pinned", a_card.pinned },
				{ "stages", a_card.stages },
				{ "estSec", SecOrNull(a_card.estSec) },
				{ "estPartial", a_card.estPartial },
				{ "openEnded", a_card.openEnded },
			};
		}

	}

	bool IsWheelEntryEligible(const Catalog::Node& a_def, std::int32_t a_stage)
	{
		return a_stage < 0 ? IsWheelScene(a_def) : WheelStage(a_def, a_stage) != nullptr;
	}

	json BuildWheelData(std::string_view a_tagPrefix)
	{
		using Serialization::WheelPins::Entry;
		struct Item
		{
			Entry        entry;
			std::string  title;
			std::int32_t priority = 0;
			std::int32_t weight = 1;
		};

		std::vector<Item> items;
		auto add = [&items](const Catalog::Node& a_def, const Entry& a_entry) {
			if (a_entry.stage < 0) {
				if (!IsWheelScene(a_def)) {
					return;
				}
				items.push_back({ a_entry, a_def.name.empty() ? a_def.id : a_def.name, a_def.priority, a_def.weight });
				return;
			}
			const auto* node = WheelStage(a_def, a_entry.stage);
			if (!node) {
				return;
			}
			const auto& stage = *node;
			std::string title = stage.name;
			if (title.empty()) {
				title = a_def.stages.size() == 1
				          ? (a_def.name.empty() ? a_def.id : a_def.name)
				          : std::format("{} · Stage {}", a_def.name.empty() ? a_def.id : a_def.name, a_entry.stage + 1);
			}
			items.push_back({ a_entry, std::move(title), a_def.priority, a_def.weight });
		};

		auto& registry = EngineClient::CatalogData::GetSingleton();
		const bool customized = Serialization::WheelPins::Customized();
		if (customized) {
			for (const auto& entry : Serialization::WheelPins::Entries()) {
				if (const auto def = registry.Find(entry.scene)) {
					add(*def, entry);
				}
			}
		} else {
			const std::string prefix = Util::ToLower(a_tagPrefix.empty() ? std::string_view{ "player.emote." } : a_tagPrefix);
			registry.ForEachDef([&](const Catalog::Node& a_def) {
				if (a_def.clipsAvailable && IsWheelScene(a_def) && std::ranges::any_of(a_def.tags,
					[&](const std::string& a_tag) { return Util::ToLower(a_tag).starts_with(prefix); })) {
					add(a_def, Entry{ a_def.id, -1 });
				}
			});
			std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
				return a.priority != b.priority ? a.priority > b.priority :
				       a.weight != b.weight ? a.weight > b.weight : a.title < b.title;
			});
			if (items.size() > 12) {
				items.resize(12);
			}
		}

		json entries = json::array();
		for (const auto& item : items) {
			json value = {
				{ "scene", item.entry.scene },
				{ "title", item.title },
			};
			if (item.entry.stage >= 0) {
				value["stage"] = item.entry.stage;
			}
			entries.push_back(std::move(value));
		}
		return { { "customized", customized }, { "entries", std::move(entries) } };
	}

	// osf.catalog.data (a_library=false) or osf.library.data (a_library=true).
	json BuildCatalog(bool a_library)
	{
		const bool wheelCustomized = Serialization::WheelPins::Customized();
		const auto wheelEntries = Serialization::WheelPins::Entries();
		const auto wheelOrder = [&wheelEntries](std::string_view a_scene, std::int32_t a_stage) {
			for (std::size_t i = 0; i < wheelEntries.size(); ++i) {
				if (wheelEntries[i].scene == a_scene && wheelEntries[i].stage == a_stage) {
					return static_cast<std::int32_t>(i) + 1;
				}
			}
			return 0;
		};
		std::vector<Card> cards;
		EngineClient::CatalogData::GetSingleton().ForEachDef(
			[&cards, &wheelOrder, a_library, wheelCustomized](const Catalog::Node& d) {
			if (d.library != a_library) {
				return;
			}
			if (!d.clipsAvailable) {
				return;
			}
			Card c;
			c.id = d.id;
			c.title = d.name.empty() ? d.id : d.name;
			c.pack = d.pack;
			c.folder = d.folder;
			c.sourceFile = d.sourceFile;
			c.species = d.species.empty() ? std::string{ "human" } : d.species;
			c.sourcePath = d.sourcePath;
			c.sourceKind = d.sourceKind;
			c.tags = d.tags;
			c.actorCount = d.actorCount;
			c.roles.reserve(d.roles.size());
			for (const auto& r : d.roles) {
				c.roles.push_back({ r.name, r.gender });
			}
			c.priority = d.priority;
			c.weight = d.weight;
			c.hideApparel = d.hideApparel;
			c.playerInputLock = d.playerInputLock;
			c.fade = d.fade;
			c.requiresFurniture = d.RequiresAnchor();
			c.worldPlacement = d.placement;
			// Base forms rarely retain an edid at runtime; fall back to the form id.
			for (const auto kwId : d.anchorKeywords) {
				if (std::string lbl = KeywordLabel(RE::TESForm::LookupByID<RE::BGSKeyword>(kwId)); !lbl.empty()) {
					c.anchorNames.push_back(std::move(lbl));
				}
			}
			for (const auto b : d.anchorBaseForms) {
				const auto* form = RE::TESForm::LookupByID(b);
				const char* edid = form ? form->GetFormEditorID() : nullptr;
				c.anchorNames.push_back(edid && edid[0] ? std::string{ edid } : std::format("{:#010x}", b));
			}
			c.unlisted = d.unlisted;
			c.wheelCustomized = wheelCustomized;
			c.pinned = wheelOrder(d.id, -1);
			// Each desugared linear node holds exactly one StageDef.
			c.stages.reserve(d.stages.size());
			for (std::size_t i = 0; i < d.stages.size(); ++i) {
				const auto* node = &d.stages[i];
				if (!node->resolved) {
					c.estPartial = true;  // `use` node: unknown time
					continue;
				}
				const auto& st = *node;
				StageCard sc;
				sc.index = static_cast<std::int32_t>(i);
				sc.name = st.name;
				sc.tags = st.tags;
				sc.clipCount = st.clipCount;
				sc.pinned = wheelOrder(d.id, sc.index);

				sc.loopSec = st.loopSec;

				// A frozen stage never plays its clip: cost is its timer, or zero when open-ended.
				if (st.hold >= 0.0f) {
					sc.timerSec = node->timerSec;
					sc.loops = 0;
					if (sc.timerSec > 0.0f) {
						sc.estSec = sc.timerSec;
					} else {
						sc.openEnded = true;
						sc.estSec = 0.0f;
					}
					c.estSec = (c.estSec < 0.0f ? 0.0f : c.estSec) + sc.estSec;
					c.openEnded = c.openEnded || sc.openEnded;
					c.stages.push_back(std::move(sc));
					continue;
				}

				sc.timerSec = node->timerSec;
				if (node->loopMode == "once") {
					sc.loops = -1;
					sc.estSec = sc.loopSec;
					if (sc.timerSec > 0.0f) {  // a timer edge can cut the pass short
						sc.estSec = sc.estSec >= 0.0f ? std::min(sc.estSec, sc.timerSec) : sc.timerSec;
					}
				} else if (node->loopMode == "count") {
					sc.loops = node->loopCount;
					if (sc.loopSec >= 0.0f) {
						sc.estSec = static_cast<float>(node->loopCount) * sc.loopSec;
						if (sc.timerSec > 0.0f) {
							sc.estSec = std::min(sc.estSec, sc.timerSec);
						}
					} else if (sc.timerSec > 0.0f) {
						sc.estSec = sc.timerSec;  // upper bound
					}
				} else {
					sc.loops = 0;
					if (sc.timerSec > 0.0f) {
						sc.estSec = sc.timerSec;
					} else {
						sc.openEnded = true;
						if (sc.loopSec >= 0.0f) {
							sc.estSec = kHoldLoopEstimate * sc.loopSec;
						}
					}
				}

				if (sc.estSec >= 0.0f) {
					c.estSec = (c.estSec < 0.0f ? 0.0f : c.estSec) + sc.estSec;
				} else {
					c.estPartial = true;
				}
				c.openEnded = c.openEnded || sc.openEnded;
				c.stages.push_back(std::move(sc));
			}
			cards.push_back(std::move(c));
		});

		{
			std::vector<std::string> titleKeys;
			titleKeys.reserve(cards.size());
			for (const auto& card : cards) {
				titleKeys.push_back(Util::ToLower(card.title));
			}
			std::vector<std::size_t> order(cards.size());
			std::iota(order.begin(), order.end(), std::size_t{ 0 });
			std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
				return titleKeys[a] != titleKeys[b] ? titleKeys[a] < titleKeys[b] : cards[a].id < cards[b].id;
			});
			std::vector<Card> sorted;
			sorted.reserve(cards.size());
			for (const auto index : order) {
				sorted.push_back(std::move(cards[index]));
			}
			cards = std::move(sorted);
		}

		json arr = cards;
		REX::DEBUG("[UI] {} built -> {} entr{}", a_library ? "library" : "catalog", cards.size(), cards.size() == 1 ? "y" : "ies");
		return arr;
	}

	// Caps reply size; the full set stays in the log and OSFAdvanced.GetSceneLoadErrors(). Counts stay exact.
	constexpr std::size_t kMaxProblemsPerFile = 12;

	// osf.animation.imports.data: per file, so files that produced no cards still appear.
	json BuildFileReport()
	{
		const auto& stats = EngineClient::CatalogData::GetSingleton().FileStats();

		json files = json::array();
		std::uint64_t totalDeclared = 0, totalNodes = 0, totalRejectedNodes = 0;
		std::uint64_t totalClipEntries = 0, totalErrors = 0, totalWarnings = 0;
		std::uint64_t totalHidden = 0, totalMissing = 0, totalBytes = 0;
		std::uint32_t rejectedFiles = 0, realFiles = 0;
		float totalMs = 0.0f;
		for (const auto& s : stats) {
			totalNodes += s.nodes;
			totalDeclared += s.declaredNodes;
			totalRejectedNodes += s.rejectedNodes;
			totalClipEntries += s.clipEntries;
			totalErrors += s.errors;
			totalWarnings += s.warnings;
			totalHidden += s.hidden;
			totalMissing += s.missingClips;
			totalBytes += s.bytes;
			totalMs += s.parseMs;
			// The trailing cross-file bucket has no path and is not a file.
			if (!s.path.empty()) {
				++realFiles;
				rejectedFiles += s.Rejected() ? 1u : 0u;
			}

			json problems = json::array();
			for (std::size_t i = 0; i < s.problems.size() && i < kMaxProblemsPerFile; ++i) {
				json problem = s.problems[i];
				problem["severity"] = s.problems[i].warning ? "warn" : "error";
				problems.push_back(std::move(problem));
			}
			files.push_back({
				{ "path", s.path },
				{ "file", s.file },
				{ "pack", s.pack },
				{ "library", s.library },
				{ "schema", s.schema },
				{ "bytes", s.bytes },
				{ "parseMs", s.parseMs },
				{ "declaredNodes", s.declaredNodes },
				{ "rejectedNodes", s.rejectedNodes },
				{ "hidden", s.hidden },
				{ "unlisted", s.unlisted },
				{ "anchored", s.anchored },
				{ "nodes", s.nodes },
				{ "stages", s.stages },
				{ "roles", s.roles },
				{ "clips", s.clips },
				{ "distinctClips", s.distinctClips },
				{ "missingClips", s.missingClips },
				{ "missingClipExamples", s.missingClipExamples },
				{ "cues", s.cues },
				{ "actions", s.actions },
				{ "sounds", s.sounds },
				{ "cameras", s.cameras },
				{ "clipEntries", s.clipEntries },
				{ "species", s.species },
				{ "errors", s.errors },
				{ "warnings", s.warnings },
				{ "rejected", s.Rejected() },
				{ "problems", std::move(problems) },
				{ "problemCount", static_cast<std::uint32_t>(s.problems.size()) },
			});
		}

		REX::DEBUG("[UI] content import report built -> {} file record(s), {} problem(s)", stats.size(), totalErrors + totalWarnings);
		return {
			{ "files", std::move(files) },
			{ "totals", {
				{ "files", realFiles },
				{ "rejectedFiles", rejectedFiles },
				{ "declaredNodes", totalDeclared },
				{ "rejectedNodes", totalRejectedNodes },
				{ "nodes", totalNodes },
				{ "registered", static_cast<std::uint64_t>(EngineClient::CatalogData::GetSingleton().Size()) },
				{ "clipEntries", totalClipEntries },
				{ "hidden", totalHidden },
				{ "missingClips", totalMissing },
				{ "errors", totalErrors },
				{ "warnings", totalWarnings },
				{ "bytes", totalBytes },
				{ "parseMs", totalMs },
			} },
		};
	}
	std::optional<std::string> BuildImportTextReport(std::string_view a_path)
	{
		const auto& stats = EngineClient::CatalogData::GetSingleton().FileStats();
		const auto found = std::find_if(stats.begin(), stats.end(), [&](const Catalog::File& a_stats) {
			return a_stats.path == a_path;
		});
		if (found == stats.end()) {
			return std::nullopt;
		}

		const auto& file = *found;
		std::string text = "OSF Animation - content import report\r\n";
		text += std::format("File: {}\r\n", file.path.empty() ? "Cross-file problems" : file.path);
		if (!file.pack.empty()) {
			text += std::format("Pack: {}\r\n", file.pack);
		}
		text += std::format("Result: {} accepted / {} declared; {} rejected; {} hidden; {} missing clips\r\n",
			file.nodes, file.declaredNodes, file.rejectedNodes, file.hidden, file.missingClips);
		text += std::format("Schema: {} | Size: {} bytes | Load: {:.2f} ms\r\n", file.schema, file.bytes, file.parseMs);
		text += std::format("Content: {} nodes | {} stages | {} roles | {} clip slots | {} distinct clips | {} library entries\r\n",
			file.nodes, file.stages, file.roles, file.clips, file.distinctClips, file.clipEntries);
		text += std::format("Tracks: {} cues | {} actions | {} sounds | {} cameras\r\n",
			file.cues, file.actions, file.sounds, file.cameras);
		if (!file.species.empty()) {
			text += "Species:";
			for (const auto& species : file.species) {
				text += " " + species;
			}
			text += "\r\n";
		}
		text += std::format("Diagnostics: {} error(s), {} warning(s)\r\n", file.errors, file.warnings);

		if (!file.missingClipExamples.empty()) {
			text += "\r\nMissing clip examples:\r\n";
			for (const auto& clip : file.missingClipExamples) {
				text += "- " + clip + "\r\n";
			}
		}
		if (!file.problems.empty()) {
			text += "\r\nProblems:\r\n";
			for (const auto& problem : file.problems) {
				text += std::format("[{}] {}\r\n", problem.code.empty() ? (problem.warning ? "warning" : "error") : problem.code,
					problem.message);
				if (!problem.node.empty() || !problem.role.empty() || !problem.clip.empty()) {
					text += std::format("  Context: node={} role={} clip={}\r\n",
						problem.node.empty() ? "-" : problem.node,
						problem.role.empty() ? "-" : problem.role,
						problem.clip.empty() ? "-" : problem.clip);
				}
				if (!problem.hint.empty()) {
					text += "  Next: " + problem.hint + "\r\n";
				}
			}
		}
		return text;
	}

}
