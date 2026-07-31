#include "API/UIBridgeCatalog.h"

#include "Registry/SceneRegistry.h"
#include "Serialization/ClipDurations.h"
#include "Serialization/WheelPins.h"
#include "Util/StringUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace OSF::API::UIBridgeCatalog
{
	using json = nlohmann::json;

	namespace
	{
		std::string KeywordLabel(RE::BGSKeyword* a_kw)
		{
			const char* edid = a_kw ? a_kw->GetFormEditorID() : nullptr;
			if (!edid || !edid[0]) {
				return {};
			}
			std::string_view sv{ edid };
			for (const std::string_view prefix : { "AnimFurn", "Anim" }) {
				if (sv.starts_with(prefix)) {
					sv.remove_prefix(prefix.size());
					break;
				}
			}
			std::string out;
			out.reserve(sv.size() + 8);
			for (std::size_t i = 0; i < sv.size(); ++i) {
				const char c = sv[i];
				// Break lower/digit->Upper ("ChairScrappy") and acronym->word ("HVACUnit" -> "HVAC Unit").
				if (i > 0 && std::isupper(static_cast<unsigned char>(c)) &&
					(!std::isupper(static_cast<unsigned char>(sv[i - 1])) ||
						(i + 1 < sv.size() && std::islower(static_cast<unsigned char>(sv[i + 1]))))) {
					out += ' ';
				}
				out += (c == '_') ? ' ' : c;
			}
			return out;
		}


		const char* GenderTag(Registry::SlotGender a_gender)
		{
			switch (a_gender) {
			case Registry::SlotGender::kMale:
				return "male";
			case Registry::SlotGender::kFemale:
				return "female";
			default:
				return "any";
			}
		}

		// Actor count for a card: the declared role count, else the first playable stage's clip count
		// (anonymous positional scenes have no roles[]). ForEachDef pins the immutable snapshot.
		std::size_t ActorCountOf(const Registry::SceneDef& a_def)
		{
			if (!a_def.roles.empty()) {
				return a_def.roles.size();
			}
			const Registry::SceneNode* node = a_def.FindNode(a_def.entry);
			if (!node && !a_def.nodes.empty()) {
				node = &a_def.nodes.front();
			}
			if (node && !node->stages.empty()) {
				return node->stages.front().clips.size();
			}
			return 0;
		}


		// How many loops an open-ended hold stage is assumed to run for the scene time estimate
		constexpr float kHoldLoopEstimate = 2.0f;
		bool IsEmote(const Registry::SceneDef& a_def)
		{
			return std::ranges::any_of(a_def.tagSet,
				[](const std::string& a_tag) { return a_tag.starts_with("player.emote."); });
		}

		bool IsWheelScene(const Registry::SceneDef& a_def)
		{
			return !a_def.library && !a_def.unlisted && a_def.roles.size() == 1 &&
			       !a_def.RequiresAnchor() && IsEmote(a_def);
		}

		const Registry::SceneNode* WheelStage(const Registry::SceneDef& a_def, std::int32_t a_stage)
		{
			if (!a_def.library || a_def.roles.size() != 1 || a_def.RequiresAnchor() ||
			    (!a_def.species.empty() && a_def.species != "human") ||
			    a_stage < 0 || static_cast<std::size_t>(a_stage) >= a_def.linearStages.size()) {
				return nullptr;
			}
			const auto* node = a_def.FindNode(a_def.linearStages[static_cast<std::size_t>(a_stage)]);
			return node && !node->stages.empty() && !node->stages.front().clips.empty() ? node : nullptr;
		}

	}

	bool IsWheelEntryEligible(const Registry::SceneDef& a_def, std::int32_t a_stage)
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
		auto add = [&items](const Registry::SceneDef& a_def, const Entry& a_entry) {
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
			const auto& stage = node->stages.front();
			std::string title = stage.name;
			if (title.empty()) {
				title = a_def.linearStages.size() == 1
				          ? (a_def.name.empty() ? a_def.id : a_def.name)
				          : std::format("{} · Stage {}", a_def.name.empty() ? a_def.id : a_def.name, a_entry.stage + 1);
			}
			items.push_back({ a_entry, std::move(title), a_def.priority, a_def.weight });
		};

		auto& registry = Registry::SceneRegistry::GetSingleton();
		const bool customized = Serialization::WheelPins::Customized();
		if (customized) {
			for (const auto& entry : Serialization::WheelPins::Entries()) {
				if (const auto def = registry.Find(entry.scene)) {
					add(*def, entry);
				}
			}
		} else {
			const std::string prefix = Util::ToLower(a_tagPrefix.empty() ? std::string_view{ "player.emote." } : a_tagPrefix);
			registry.ForEachDef([&](const Registry::SceneDef& a_def) {
				if (a_def.clipsAvailable && IsWheelScene(a_def) && std::ranges::any_of(a_def.tagSet,
					[&](const std::string& a_tag) { return a_tag.starts_with(prefix); })) {
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

	// Serialize the live scene registry to the osf.catalog.data array (a_library=false) or the osf.library.data array (a_library=true — the reference-library lane, e.g. the generated vanilla packs).
	// Copies fields from the pinned registry snapshot, then builds JSON afterwards.
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
		struct StageCard
		{
			std::int32_t             index = 0;
			std::string              name;   // stage label ("" = unlabeled)
			std::vector<std::string> tags;
			std::int32_t             clipCount = 0;
			std::int32_t             pinned = 0;  // 1-based animation-wheel order
			// Timing. loopSec = the clip's loop length (the honest per-animation number);
			// estSec folds in the stage's loops/timer; either < 0 = unknown (clip not probed yet).
			float                    loopSec = -1.0f;
			float                    timerSec = 0.0f;   // auto-advance timer (0 = none)
			std::int32_t             loops = -1;        // -1 = play once, 0 = hold, N = loop count
			bool                     openEnded = false; // hold with no timer: runs until advanced
			float                    estSec = -1.0f;
		};
		struct Card
		{
			std::string              id;
			std::string              title;
			std::string              pack;        // file-level `pack` label — the browser's group-by-pack key ("" = none authored)
			std::string              folder;      // optional slash-delimited catalog path within the pack
			std::string              sourceFile;  // scene file name only (no directories) — the browser's grouping fallback
			std::string              species;  // skeleton family ("human" default) for the browser's per-actor filter
			std::vector<std::string> tags;
			std::uint32_t            actorCount = 0;
			std::vector<std::string> genders;
			std::vector<std::string> roleNames;  // authored role names ("" for anonymous slots) — labels/search only, binding stays positional
			std::int32_t             priority = 0;
			std::int32_t             weight = 1;
			bool                     stripActors = true;
			bool                     clearHeldItems = true;
			bool                     lockPlayer = true;
			bool                     fade = false;
			bool                     requiresFurniture = false;
			bool                     inPlace = false;
			std::vector<std::string> anchorNames;  // human labels for WHAT the scene anchors to ("Barstool", ...)
			bool                     unlisted = false;
			// Generated one-clip entry that a pack REGISTERED via `clipLibrary`, as opposed to
			// one harvested from a scene's stages. Both carry the `osf.scene-clip/` id, so the
			// browser cannot tell authored content from its own debug surface without this.
			bool                     curated = false;
			std::int32_t             pinned = 0;  // 1-based explicit wheel order (0 = absent/default-derived)
			std::vector<StageCard>   stages;  // linear stages, in order (empty for a non-linear graph)
			float                    estSec = -1.0f;      // sum of known stage estimates (< 0 = none known)
			bool                     estPartial = false;  // at least one linear stage had no estimate
			bool                     openEnded = false;   // some stage holds until advanced
		};
		std::vector<Card> cards;
		Registry::SceneRegistry::GetSingleton().ForEachDef([&cards, &wheelOrder, a_library](const Registry::SceneDef& d) {
			if (d.library != a_library) {
				return;  // each lane serializes only its own scenes
			}
			if (!d.clipsAvailable) {
				return;  // clips not installed (compat pack without its source mod) — unplayable, keep it off the shelf
			}
			Card c;
			c.id = d.id;
			c.title = d.name.empty() ? d.id : d.name;
			c.pack = d.pack;
			c.folder = d.folder;
			// Filename only: the view groups by it when no `pack` is authored, and a full
			// path would leak the user's install location into the overlay.
			const auto srcName = d.sourceFile.filename().u8string();
			c.sourceFile.assign(srcName.begin(), srcName.end());
			c.species = d.species.empty() ? std::string{ "human" } : d.species;
			c.tags = d.tags;
			c.actorCount = static_cast<std::uint32_t>(ActorCountOf(d));
			c.genders.reserve(d.roles.size());
			c.roleNames.reserve(d.roles.size());
			for (const auto& r : d.roles) {
				c.genders.emplace_back(GenderTag(r.gender));
				c.roleNames.emplace_back(r.name);
			}
			c.priority = d.priority;
			c.weight = d.weight;
			c.stripActors = d.stripActors;
			c.clearHeldItems = d.clearHeldItems;
			c.lockPlayer = d.lockPlayer;
			c.fade = d.fade;
			c.requiresFurniture = d.RequiresAnchor();
			c.inPlace = d.inPlace;
			// Name the anchor, not just the fact of one: keyword edids prettify well
			// ("AnimFurnBarstool" -> "Barstool"); base-form anchors rarely retain an edid,
			// so those fall back to the form id — still identifiable, never blank.
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
			c.curated = d.curatedClip;
			c.pinned = wheelOrder(d.id, -1);
			// Enumerate the scene's linear stages as browsable animations (each desugared node holds exactly one StageDef).
			c.stages.reserve(d.linearStages.size());
			for (std::size_t i = 0; i < d.linearStages.size(); ++i) {
				const auto* node = d.FindNode(d.linearStages[i]);
				if (!node || node->stages.empty()) {
					c.estPartial = true;  // a `use` node contributes unknown time
					continue;
				}
				const auto& st = node->stages.front();
				StageCard sc;
				sc.index = static_cast<std::int32_t>(i);
				sc.name = st.name;
				sc.tags = st.tags;
				sc.clipCount = static_cast<std::int32_t>(st.clips.size());
				sc.pinned = wheelOrder(d.id, sc.index);

				// Stage timing, from the node the desugar produce: loop length comes from clips[0].
				// A pack-authored duration wins over the probe cache (generated vanilla packs).
				if (!st.clips.empty()) {
					const auto& first = st.clips.front();
					if (first.sec > 0.0f) {
						sc.loopSec = first.sec;
					} else if (const auto sec = Serialization::ClipDurations::Lookup(first.file, first.animId)) {
						sc.loopSec = *sec;
					}
				}
				sc.timerSec = node->timerSec;
				switch (node->loopMode) {
				case Registry::LoopMode::kOnce:
					sc.loops = -1;
					sc.estSec = sc.loopSec;  // one pass ends the stage
					if (sc.timerSec > 0.0f) {  // hand-authored node: a timer edge can cut the pass short
						sc.estSec = sc.estSec >= 0.0f ? std::min(sc.estSec, sc.timerSec) : sc.timerSec;
					}
					break;
				case Registry::LoopMode::kCount:
					sc.loops = node->loopCount;
					if (sc.loopSec >= 0.0f) {
						sc.estSec = static_cast<float>(node->loopCount) * sc.loopSec;
						if (sc.timerSec > 0.0f) {
							sc.estSec = std::min(sc.estSec, sc.timerSec);  // whichever fires first
						}
					} else if (sc.timerSec > 0.0f) {
						sc.estSec = sc.timerSec;  // upper bound: the timer caps the stage
					}
					break;
				case Registry::LoopMode::kHold:
					sc.loops = 0;
					if (sc.timerSec > 0.0f) {
						sc.estSec = sc.timerSec;  // timed hold: exact
					} else {
						sc.openEnded = true;  // runs until advanced — assume a couple of loops
						if (sc.loopSec >= 0.0f) {
							sc.estSec = kHoldLoopEstimate * sc.loopSec;
						}
					}
					break;
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

		std::sort(cards.begin(), cards.end(), [](const Card& a, const Card& b) {
			const auto la = Util::ToLower(a.title), lb = Util::ToLower(b.title);
			return la != lb ? la < lb : a.id < b.id;
		});

		// Unknown durations serialize as null (never a sentinel the view could mistake for seconds).
		const auto secOrNull = [](float a_sec) { return a_sec >= 0.0f ? json(a_sec) : json(nullptr); };

		json arr = json::array();
		for (const auto& c : cards) {
			json stages = json::array();
			for (const auto& s : c.stages) {
				stages.push_back({
					{ "index", s.index },
					{ "name", s.name },
					{ "tags", s.tags },
					{ "clipCount", s.clipCount },
					{ "pinned", s.pinned },
					{ "loopSec", secOrNull(s.loopSec) },
					{ "timerSec", s.timerSec > 0.0f ? json(s.timerSec) : json(nullptr) },
					{ "loops", s.loops >= 0 ? json(s.loops) : json(nullptr) },
					{ "openEnded", s.openEnded },
					{ "estSec", secOrNull(s.estSec) },
				});
			}
			arr.push_back({
				{ "id", c.id },
				{ "title", c.title },
				{ "pack", c.pack },
				{ "folder", c.folder },
				{ "sourceFile", c.sourceFile },
				{ "species", c.species },
				{ "tags", c.tags },
				{ "actorCount", c.actorCount },
				{ "genders", c.genders },
				{ "roles", [&c]() {
					 json roles = json::array();
					 for (std::size_t i = 0; i < c.roleNames.size(); i++) {
						 roles.push_back({ { "name", c.roleNames[i] },
							 { "gender", i < c.genders.size() ? c.genders[i] : "any" } });
					 }
					 return roles;
				 }() },
				{ "priority", c.priority },
				{ "weight", c.weight },
				{ "stripActors", c.stripActors },
				{ "clearHeldItems", c.clearHeldItems },
				{ "lockPlayer", c.lockPlayer },
				{ "fade", c.fade },
				{ "requiresFurniture", c.requiresFurniture },
				{ "inPlace", c.inPlace },
				{ "anchors", c.anchorNames },
				{ "unlisted", c.unlisted },
				{ "curated", c.curated },
				{ "wheelCustomized", wheelCustomized },
				{ "pinned", c.pinned },
				{ "stageCount", static_cast<std::int32_t>(c.stages.size()) },
				{ "stages", std::move(stages) },
				{ "estSec", secOrNull(c.estSec) },
				{ "estPartial", c.estPartial },
				{ "openEnded", c.openEnded },
			});
		}
		REX::DEBUG("[UI] {} built -> {} entr{}", a_library ? "library" : "catalog", cards.size(), cards.size() == 1 ? "y" : "ies");
		return arr;
	}

	// At most this many problem lines travel per file. The full set stays in the log and in
	// OSF.GetSceneLoadErrors(); a pack with 300 bad scenes must not turn one reply into a
	// megabyte of text the panel cannot render anyway. The counts are always exact.
	constexpr std::size_t kMaxProblemsPerFile = 12;

	// Serialize the registry's per-file import records to osf.animation.imports.data. This is a
	// FILE-shaped view of the load, deliberately unlike the catalog's scene-shaped one: a file
	// that produced nothing has no scene to appear as, and "my pack didn't load" is precisely
	// the question the catalog cannot answer.
	json BuildFileReport()
	{
		const auto stats = Registry::SceneRegistry::GetSingleton().FileStats();

		json files = json::array();
		std::uint64_t totalScenes = 0, totalClipEntries = 0, totalErrors = 0, totalWarnings = 0;
		std::uint64_t totalHidden = 0, totalMissing = 0, totalBytes = 0;
		std::uint32_t rejectedFiles = 0, realFiles = 0;
		float totalMs = 0.0f;
		for (const auto& s : stats) {
			totalScenes += s.scenes;
			totalClipEntries += s.clipEntries;
			totalErrors += s.errors;
			totalWarnings += s.warnings;
			totalHidden += s.hidden;
			totalMissing += s.missingClips;
			totalBytes += s.bytes;
			totalMs += s.parseMs;
			// The trailing cross-file bucket has no path and is not a file — it must not count
			// toward "N files scanned" or it reads as a phantom pack.
			if (!s.path.empty()) {
				++realFiles;
				rejectedFiles += s.Rejected() ? 1u : 0u;
			}

			json problems = json::array();
			for (std::size_t i = 0; i < s.problems.size() && i < kMaxProblemsPerFile; ++i) {
				problems.push_back(s.problems[i]);
			}
			files.push_back({
				{ "path", s.path },
				{ "file", s.file },
				{ "pack", s.pack },
				{ "library", s.library },
				{ "schema", s.schema },
				{ "bytes", s.bytes },
				{ "parseMs", s.parseMs },
				{ "scenes", s.scenes },
				{ "hidden", s.hidden },
				{ "unlisted", s.unlisted },
				{ "anchored", s.anchored },
				{ "nodes", s.nodes },
				{ "stages", s.stages },
				{ "roles", s.roles },
				{ "clips", s.clips },
				{ "distinctClips", s.distinctClips },
				{ "missingClips", s.missingClips },
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
				// So the panel can say "12 of 40 shown" instead of silently truncating.
				{ "problemCount", static_cast<std::uint32_t>(s.problems.size()) },
			});
		}

		REX::DEBUG("[UI] import report built -> {} file record(s), {} problem(s)", stats.size(), totalErrors + totalWarnings);
		return {
			{ "files", std::move(files) },
			{ "totals", {
				{ "files", realFiles },
				{ "rejectedFiles", rejectedFiles },
				{ "scenes", totalScenes },
				// The registry's own authored count, so a mismatch with the per-file sum is
				// visible rather than silently averaged away.
				{ "registered", static_cast<std::uint64_t>(Registry::SceneRegistry::GetSingleton().Size()) },
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

}