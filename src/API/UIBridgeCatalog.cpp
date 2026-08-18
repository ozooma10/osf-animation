#include "API/UIBridgeCatalog.h"
#include "API/UIKeywordLabel.h"

#include "Registry/ContentRegistry.h"
#include "Serialization/ClipDurations.h"
#include "Serialization/WheelPins.h"
#include "Util/StringUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace OSF::Registry
{
	// Seven of the eight wire keys ARE the member names. Deliberately defined in this TU and not
	// in SceneRegistry.h: a repo-wide serializer would let a caller emit the untruncated `problems`
	// vector, which is exactly what kMaxProblemsPerFile exists to prevent. `warning` is renamed to
	// `severity` by the one caller (BuildFileReport).
	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(SceneImportProblem,
		code, message, hint, scene, node, role, clip)
}

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

		struct TrackMark
		{
			std::string kind;
			std::string trackPosition;
			std::string label;
			std::string detail;
			std::string role;
			float       at = 0.0f;
			float       atSec = -1.0f;  // authored `atFrame` position in seconds (< 0 = `at`-fraction mark)
			bool        repeat = false;
		};

		void to_json(json& a_out, const TrackMark& a_mark)
		{
			a_out = {
				{ "kind", a_mark.kind },
				{ "at", a_mark.at },
				{ "trackPosition", a_mark.trackPosition },
				{ "anchor", a_mark.trackPosition },  // legacy bridge field
				{ "label", a_mark.label },
				{ "detail", a_mark.detail },
				{ "role", a_mark.role },
				{ "repeat", a_mark.repeat },
			};
			if (a_mark.atSec >= 0.0f) {
				a_out["atSec"] = a_mark.atSec;  // authored `atFrame` position in seconds
			}
		}

		struct StageCard
		{
			std::int32_t             index = 0;
			std::string              name;   // stage label ("" = unlabeled)
			std::vector<std::string> tags;
			std::vector<TrackMark>   tracks;
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

		void to_json(json& a_out, const StageCard& a_stage)
		{
			a_out = {
				{ "index", a_stage.index },
				{ "name", a_stage.name },
				{ "tags", a_stage.tags },
				{ "clipCount", a_stage.clipCount },
				{ "pinned", a_stage.pinned },
				{ "tracks", a_stage.tracks },
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
			std::string              sourcePath;  // Data/OSF-relative path for exact Imports -> catalog navigation
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
			// Generated one-clip entry that a pack REGISTERED via `clipLibrary`, as opposed to
			// one harvested from a scene's stages. Both carry the `osf.scene-clip/` id, so the
			// browser cannot tell authored content from its own debug surface without this.
			bool                     curated = false;
			bool                     wheelCustomized = false;  // whole-wheel state, mirrored onto every card
			std::int32_t             pinned = 0;  // 1-based explicit wheel order (0 = absent/default-derived)
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
				{ "sourcePath", a_card.sourcePath },
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
				{ "curated", a_card.curated },
				{ "wheelCustomized", a_card.wheelCustomized },
				{ "pinned", a_card.pinned },
				{ "stages", a_card.stages },
				{ "estSec", SecOrNull(a_card.estSec) },
				{ "estPartial", a_card.estPartial },
				{ "openEnded", a_card.openEnded },
			};
		}

	}

	bool IsWheelEntryEligible(const Registry::SceneDef& a_def, std::int32_t a_stage)
	{
		return a_stage < 0 ? IsWheelScene(a_def) : WheelStage(a_def, a_stage) != nullptr;
	}

	nlohmann::json BuildRoutes()
	{
		auto layerJson = [](const Registry::RouteLayer& a_layer) {
			const char* mode = "override";
			switch (a_layer.mode) {
			case Animation::PoseMode::kAdditive:
				mode = "additive";
				break;
			default:
				break;
			}
			return json{
				{ "clip", a_layer.clip.file },
				{ "animId", a_layer.clip.animId },
				{ "durationHint", a_layer.clip.sec },
				{ "mask", a_layer.mask },
				{ "mode", mode },
				{ "weight", a_layer.weight },
				{ "holdAt", a_layer.holdAt },
			};
		};
		auto lifetimeName = [](Registry::RouteLifetime a_lifetime) {
			switch (a_lifetime) {
			case Registry::RouteLifetime::kStation: return "station";
			case Registry::RouteLifetime::kController: return "controller";
			case Registry::RouteLifetime::kExternal: return "external";
			default: return "transition";
			}
		};

		json routes = json::array();
		Registry::ContentRegistry::GetSingleton().ForEachRoute([&](const Registry::RouteDef& a_route) {
			json stations = json::array();
			for (const auto& station : a_route.stations) {
				json item = { { "id", station.id } };
				item["layer"] = station.layer ? layerJson(*station.layer) : json(nullptr);
				stations.push_back(std::move(item));
			}

			json transitions = json::array();
			for (const auto& transition : a_route.transitions) {
				json markers = json::array();
				for (const auto& marker : transition.markers) {
					markers.push_back({ { "frame", marker.frame }, { "id", marker.id } });
				}
				json props = json::array();
				for (const auto& prop : transition.props) {
					props.push_back({
						{ "frame", prop.frame },
						{ "id", prop.id },
						{ "attach", prop.attach },
						{ "lifetime", lifetimeName(prop.lifetime) },
						{ "attachmentNode", prop.attachment.targetNode },
						{ "node", prop.attachment.targetNode },  // legacy bridge field
					});
				}
				json sounds = json::array();
				for (const auto& sound : transition.sounds) {
					sounds.push_back({ { "frame", sound.frame }, { "spec", sound.spec } });
				}
				json item = {
					{ "id", transition.id },
					{ "from", transition.from },
					{ "to", transition.to },
					{ "layer", layerJson(transition.layer) },
					{ "markers", std::move(markers) },
					{ "props", std::move(props) },
					{ "sounds", std::move(sounds) },
					{ "interruption", transition.interruption == Registry::RouteInterruption::kCrossfadeBeforeCommit ?
						"crossfade-before-commit" : "finish" },
				};
				item["commit"] = transition.commit ? json{
					{ "frame", transition.commit->frame }, { "id", transition.commit->id }
				} : json(nullptr);
				transitions.push_back(std::move(item));
			}

			routes.push_back({
				{ "id", a_route.id },
				{ "sourceFile", a_route.sourceFile.generic_string() },
				{ "stations", std::move(stations) },
				{ "transitions", std::move(transitions) },
			});
		});
		std::sort(routes.begin(), routes.end(), [](const json& a_lhs, const json& a_rhs) {
			return a_lhs.value("id", "") < a_rhs.value("id", "");
		});
		return routes;
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

		auto& registry = Registry::ContentRegistry::GetSingleton();
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
		std::vector<Card> cards;
		std::error_code sourceRootEc;
		const auto sourceRoot = std::filesystem::current_path(sourceRootEc) / "Data" / "OSF";
		Registry::ContentRegistry::GetSingleton().ForEachDef(
			[&cards, &wheelOrder, &sourceRoot, sourceRootEc, a_library, wheelCustomized](const Registry::SceneDef& d) {
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
			c.sourcePath = c.sourceFile;
			c.sourceKind = Registry::CatalogSourceKindName(d.sourceKind);
			if (!sourceRootEc) {
				const auto relative = d.sourceFile.lexically_relative(sourceRoot);
				const auto text = relative.generic_string();
				if (!text.empty() && text != ".." && !text.starts_with("../")) {
					c.sourcePath = text;
				}
			}
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
			c.curated = d.curatedClip;
			c.wheelCustomized = wheelCustomized;
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
				// Resolved before the track lanes: an `atFrame` mark needs it to report a fraction.
				if (!st.clips.empty()) {
					const auto& first = st.clips.front();
					if (first.sec > 0.0f) {
						sc.loopSec = first.sec;
					} else if (const auto sec = Serialization::ClipDurations::Lookup(first.file, first.animId)) {
						sc.loopSec = *sec;
					}
				}

				// The browser's track axis is fractional, so an `atFrame` entry is placed against the
				// stage's own clip length; a stage whose length hasn't been probed yet parks them at
				// the end of the axis rather than claiming they sit at the clip start. The authored
				// position is shipped in seconds too (`atSec`) so the view can re-place the mark
				// against the LIVE decoded duration during inspection — the authored `sec` and the
				// decoded length can disagree. A frame at or past the clip end is anchored
				// "unreachable": the runtime provably never fires it (see Registry::TrackFires).
				const float clipSec = sc.loopSec;
				const auto addTrack = [&sc, clipSec](std::string kind, const auto& entry, std::string label,
					std::string detail = {}, std::string role = {}) {
					const char* trackPosition = "fraction";
					float at = 1.0f;
					switch (entry.pos) {
					case Registry::TrackPos::kEnter:
						trackPosition = "enter";
						at = 0.0f;
						break;
					case Registry::TrackPos::kExit:
						trackPosition = "exit";
						break;
					case Registry::TrackPos::kEnd:
						trackPosition = "end";
						break;
					case Registry::TrackPos::kFraction:
						at = Registry::TrackFraction(entry, clipSec);
						if (!Registry::TrackFires(entry, clipSec)) {
							trackPosition = "unreachable";
						}
						break;
					}
					sc.tracks.push_back({ std::move(kind), trackPosition, std::move(label), std::move(detail),
						std::move(role), at, Registry::TrackSeconds(entry), entry.everyLoop });
				};
				for (const auto& cue : node->cues) {
					addTrack("cue", cue, cue.id);
				}
				for (const auto& action : node->actions) {
					std::string detail = !action.prop.empty() ? action.prop :
						!action.set.empty() ? action.set : action.item;
					addTrack("action", action, action.type, std::move(detail), action.role);
				}
				for (const auto& sound : node->sounds) {
					addTrack("sound", sound, sound.spec, {}, sound.role);
				}
				for (const auto& camera : node->cameras) {
					std::string detail;
					if (camera.distance != 0.0f) {
						detail = std::format("distance {}", camera.distance);
					}
					addTrack("camera", camera, std::string(Registry::CameraStateName(camera.state)), std::move(detail));
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
		return arr;
	}

	// At most this many problem lines travel per file. The full set stays in the log and in
	// OSFAdvanced.GetSceneLoadErrors(); a pack with 300 bad scenes must not turn one reply into a
	// megabyte of text the panel cannot render anyway. The counts are always exact.
	constexpr std::size_t kMaxProblemsPerFile = 12;

	// Serialize the registry's per-file import records to osf.animation.imports.data. This is a
	// FILE-shaped view of the load, deliberately unlike the catalog's card-shaped one: a file
	// that produced no cards still needs to appear, and "my pack didn't load" is precisely
	// the question the catalog cannot answer.
	json BuildFileReport()
	{
		const auto stats = Registry::ContentRegistry::GetSingleton().FileStats();

		json files = json::array();
		std::uint64_t totalDeclared = 0, totalScenes = 0, totalRejectedScenes = 0;
		std::uint64_t totalDeclaredRoutes = 0, totalRoutes = 0, totalRejectedRoutes = 0;
		std::uint64_t totalClipEntries = 0, totalErrors = 0, totalWarnings = 0;
		std::uint64_t totalHidden = 0, totalMissing = 0, totalBytes = 0;
		std::uint32_t rejectedFiles = 0, realFiles = 0;
		float totalMs = 0.0f;
		for (const auto& s : stats) {
			totalScenes += s.scenes;
			totalDeclared += s.declaredScenes;
			totalRejectedScenes += s.rejectedScenes;
			totalRoutes += s.routes;
			totalDeclaredRoutes += s.declaredRoutes;
			totalRejectedRoutes += s.rejectedRoutes;
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
				// `warning` is the only field the wire renames rather than copies.
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
				{ "declaredScenes", s.declaredScenes },
				{ "rejectedScenes", s.rejectedScenes },
				{ "scenes", s.scenes },
				{ "declaredRoutes", s.declaredRoutes },
				{ "rejectedRoutes", s.rejectedRoutes },
				{ "routes", s.routes },
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
				// So the panel can say "12 of 40 shown" instead of silently truncating.
				{ "problemCount", static_cast<std::uint32_t>(s.problems.size()) },
			});
		}

		REX::DEBUG("[UI] content import report built -> {} file record(s), {} problem(s)", stats.size(), totalErrors + totalWarnings);
		return {
			{ "files", std::move(files) },
			{ "totals", {
				{ "files", realFiles },
				{ "rejectedFiles", rejectedFiles },
				{ "declaredScenes", totalDeclared },
				{ "rejectedScenes", totalRejectedScenes },
				{ "scenes", totalScenes },
				{ "declaredRoutes", totalDeclaredRoutes },
				{ "rejectedRoutes", totalRejectedRoutes },
				{ "routes", totalRoutes },
				// The registry's own authored count, so a mismatch with the per-file sum is
				// visible rather than silently averaged away.
				{ "registered", static_cast<std::uint64_t>(Registry::ContentRegistry::GetSingleton().Size()) },
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
		const auto stats = Registry::ContentRegistry::GetSingleton().FileStats();
		const auto found = std::find_if(stats.begin(), stats.end(), [&](const Registry::ContentFileStats& a_stats) {
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
			file.scenes, file.declaredScenes, file.rejectedScenes, file.hidden, file.missingClips);
		text += std::format("Routes: {} accepted / {} declared; {} rejected\r\n",
			file.routes, file.declaredRoutes, file.rejectedRoutes);
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
				if (!problem.scene.empty() || !problem.node.empty() || !problem.role.empty() || !problem.clip.empty()) {
					text += std::format("  Context: scene={} node={} role={} clip={}\r\n",
						problem.scene.empty() ? "-" : problem.scene,
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
