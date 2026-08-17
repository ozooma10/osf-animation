#include "API/UIBridgeCatalog.h"
#include "API/UIKeywordLabel.h"

#include "Registry/ContentRegistry.h"
#include "Serialization/ClipDurations.h"
#include "Util/Profile.h"
#include "Util/StringUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace OSF::API::UIBridgeCatalog
{
	using json = nlohmann::json;

	namespace
	{
		const char* GenderTag(Registry::RoleGender a_gender)
		{
			switch (a_gender) {
			case Registry::RoleGender::kMale:
				return "male";
			case Registry::RoleGender::kFemale:
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
		// Unknown durations serialize as null, never a sentinel the view could mistake for seconds.
		json SecOrNull(float a_sec)
		{
			return a_sec >= 0.0f ? json(a_sec) : json(nullptr);
		}

		// The catalog's wire shape. BuildCatalog gathers into these from the pinned registry
		// snapshot and sorts them; each to_json below is the ONLY place its wire keys are spelled.
		// The legacy bridge aliases (stripActors/lockPlayer/inPlace/anchor) are duplicates of a
		// canonical field and sit here together rather than buried in a 38-line initializer.
		struct RoleCard
		{
			std::string name;
			std::string gender;
		};
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(RoleCard, name, gender)

		struct StageCard
		{
			std::int32_t             index = 0;
			std::string              name;   // stage label ("" = unlabeled)
			std::vector<std::string> tags;
			std::int32_t             clipCount = 0;
			// Timing. loopSec = the clip's loop length (the honest per-animation number);
			// estSec folds in the stage's loops/timer; either < 0 = unknown (clip not probed yet).
			float                    loopSec = -1.0f;
			float                    timerSec = 0.0f;   // auto-advance timer (0 = none)
			std::int32_t             loops = -1;        // -1 = play once, 0 = hold, N = loop count
			bool                     openEnded = false; // hold with no timer: runs until advanced
			float                    estSec = -1.0f;
		};

		void to_json(json& a_out, const StageCard& a_stage)
		{
			a_out = {
				{ "index", a_stage.index },
				{ "name", a_stage.name },
				{ "tags", a_stage.tags },
				{ "clipCount", a_stage.clipCount },
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
			std::string              pack;        // file-level `pack` label — the browser's group-by-pack key ("" = none authored)
			std::string              folder;      // optional slash-delimited catalog path within the pack
			std::string              sourceFile;  // scene file name only (no directories) — the browser's grouping fallback
			std::string              sourceKind;  // explicit catalog taxonomy; legacy booleans remain below
			std::string              species;  // skeleton family ("human" default) for the browser's per-actor filter
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
			std::vector<std::string> anchorNames;  // human labels for WHAT the scene anchors to ("Barstool", ...)
			bool                     unlisted = false;
			std::vector<StageCard>   stages;  // linear stages, in order (empty for a non-linear graph)
			float                    estSec = -1.0f;      // sum of known stage estimates (< 0 = none known)
			bool                     estPartial = false;  // at least one linear stage had no estimate
			bool                     openEnded = false;   // some stage holds until advanced
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
				{ "sourceKind", a_card.sourceKind },
				{ "actorCount", a_card.actorCount },
				{ "roles", a_card.roles },
				{ "priority", a_card.priority },
				{ "weight", a_card.weight },
				{ "hideApparel", a_card.hideApparel },
				{ "stripActors", a_card.hideApparel },  // legacy bridge field
				{ "playerInputLock", a_card.playerInputLock },
				{ "lockPlayer", a_card.playerInputLock },  // legacy bridge field
				{ "fade", a_card.fade },
				{ "requiresFurniture", a_card.requiresFurniture },
				{ "placement", a_card.worldPlacement },
				{ "inPlace", a_card.worldPlacement == "followActor" },  // legacy bridge field
				{ "anchors", a_card.anchorNames },
				{ "unlisted", a_card.unlisted },
				{ "stages", a_card.stages },
				{ "estSec", SecOrNull(a_card.estSec) },
				{ "estPartial", a_card.estPartial },
				{ "openEnded", a_card.openEnded },
			};
		}

	}

	// Serialize the live scene registry to the osf.catalog.data array (a_library=false) or the osf.library.data array (a_library=true — the reference-library lane, e.g. the generated vanilla packs).
	// Copies fields from the pinned registry snapshot, then builds JSON afterwards.
	json BuildCatalog(bool a_library)
	{
		OSF_PROFILE_SCOPE_N("UI.BuildCatalog");

		std::vector<Card> cards;
		Registry::ContentRegistry::GetSingleton().ForEachDef(
			[&cards, a_library](const Registry::SceneDef& d) {
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
			c.sourceKind = Registry::CatalogSourceKindName(d.sourceKind);
			c.tags = d.tags;
			c.actorCount = static_cast<std::uint32_t>(ActorCountOf(d));
			c.roles.reserve(d.roles.size());
			for (const auto& r : d.roles) {
				c.roles.push_back({ r.name, GenderTag(r.gender) });
			}
			c.priority = d.priority;
			c.weight = d.weight;
			c.hideApparel = d.hideApparel;
			c.playerInputLock = d.playerInputLock;
			c.fade = d.fade;
			c.requiresFurniture = d.RequiresAnchor();
			c.worldPlacement = d.worldPlacement == Animation::WorldPlacementMode::kFollowActor ?
				"followActor" : "anchorAndPin";
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

				// Stage timing, from the node the desugar produce: loop length comes from clips[0].
				// A pack-authored duration wins over the probe cache (generated vanilla packs).
				// Resolved before the track lanes: an `atFrame` mark needs it to report a fraction.
				if (!st.clips.empty()) {
					const auto& first = st.clips.front();
					if (first.sec > 0.0f) {
						sc.loopSec = first.sec;
					} else if (const auto sec = Serialization::ClipDurations::Lookup(first.file, first.animId)) {
						sc.loopSec = *sec;
					}
				}

				// A frozen stage never plays its clip, so the clip length is not time this stage
				// spends: its cost is its timer, or nothing at all when it holds until advanced.
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

		json arr = cards;  // the wire shape lives on the card structs (to_json, above)
		REX::DEBUG("[UI] {} built -> {} entr{}", a_library ? "library" : "catalog", cards.size(), cards.size() == 1 ? "y" : "ies");
		OSF_PROFILE_PLOT(a_library ? "UI.LibraryEntries" : "UI.CatalogEntries",
			static_cast<std::int64_t>(cards.size()));
		return arr;
	}

}
