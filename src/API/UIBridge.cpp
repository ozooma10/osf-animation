#include "API/UIBridge.h"

#include "API/OSFSceneAPI.h"  // OSFStartOptions + IOSFSceneAPI + kOSFSceneAPIVersion (in-process launch)
#include "API/OSFUI_API.h"    // the OSF UI bridge surface (JSON text only)
#include "Camera/CameraService.h"  // browse orbit: osf.orbit engages drag-to-look when no scene camera is live
#include "Input/InputService.h"  // osf.opened/closed -> UI-cursor mode for the orbit camera's drag-steer
#include "Matchmaking/Matchmaker.h"  // AnchorAccepts (osf.anchorMatch single-ref check)
#include "Registry/SceneRegistry.h"
#include "Scene/AnchorResolve.h"  // rendered-world reference anchors + in-front-of-player placement
#include "Scene/SceneRuntime.h"  // ListScenes + SetSceneObserver (the browser's ACTIVE-list push)
#include "Serialization/ClipDurations.h"  // clip loop lengths for the catalog's time estimates
#include "Serialization/WheelPins.h"  // ordered animation-wheel customization
#include "UI/HudMessage.h"    // OpenWheel's graceful-degrade popup (OSF UI absent/too old)
#include "Util/Species.h"     // catalog species tag + picked-actor species (creature filtering)
#include "Util/StringUtil.h"  // Util::ToLower

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

// The in-process handle to OSF Animation's own exported scene API.
extern "C" OSF::API::IOSFSceneAPI* OSF_RequestSceneAPI(std::uint32_t a_abiVersion);

namespace OSF::API
{
	namespace
	{
		using json = nlohmann::json;

		// The version-gated bridge wrapper (OSFUI::API::Client, header 1.7),
		// initialized once at Install; unconnected => OSF UI absent (UI
		// disabled) and every call degrades to a no-op. Static lifetime:
		// registered handlers may fire for the remaining process life.
		OSFUI::API::Client g_ui;

		// Set by OnBridgeReady; unsolicited pushes (PushCatalogUpdate) are dropped before then.
		// Only touched on the game main thread (ready callback, command handlers, SFSE tasks).
		bool g_uiReady = false;

		// Browser visibility as reported by the view (osf.opened / osf.closed). Gates OnOrbit:
		// the view batches drag deltas per animation frame, so a flush queued during the last
		// drag can arrive AFTER osf.closed — without the gate that late message would lazily
		// RE-engage the browse orbit on a closed browser, and nothing would ever release it
		// (the classic "camera stuck orbiting after closing the browser"). Game main thread only.
		bool g_viewVisible = false;
		// User preference replayed by UISettings. Defaults to the schema default when OSF UI is
		// absent/old; only browser launches consume it (wheel launches always close).
		bool g_browserAutoMinimize = true;

		// The in-space "orbit unavailable" notice fired this browser session (OnOrbit runs per
		// drag-delta batch while the orbit stays disengaged — the view must not be spammed).
		// Reset on osf.closed. Game main thread only, like g_viewVisible.
		bool g_orbitSpaceNoticed = false;

		// OSF Animation's own scene API, fetched lazily on first launch/stop.
		IOSFSceneAPI* g_scene = nullptr;

		// Last scene handle we launched, so an osf.stop with no handle can target it.
		std::int32_t g_lastHandle = 0;

		// PLAYER-cast scenes launched from the browser console, aborted when the browser
		// closes: once the UI is gone there is no stop button left, so a player mid-scene
		// would be stuck. NPC-only scenes are deliberately NOT tracked — they outlive the
		// browser (vignettes / machinima; the player can just walk away), and the ACTIVE
		// list on reopen is their stop surface. Every player-affecting mechanism (control
		// lock, camera, fade) is already engine-gated on the player being a participant, so
		// an NPC-only scene can never strand a lock. Wheel launches are NOT tracked either —
		// the wheel closes itself right after a successful pick, and the emote must survive
		// that close. Stale entries are harmless (handles are generational; StopScene on an
		// ended scene returns false) and the list is cleared on every close, so it never
		// outgrows one browser session. Main thread only.
		std::vector<std::int32_t> g_closeStops;

		// Pending animation-wheel open (OpenWheel): the osf.mode push must survive the open race,
		// so it is re-sent from the osf.opened handler while this is active. Cleared on
		// osf.closed and by OpenBrowser (a normal open must never land in wheel mode).
		// Game main thread only, like g_tokens.
		struct PendingWheel
		{
			bool         active = false;
			std::string  tagPrefix;
			std::int32_t targetToken = 0;  // 0 = player-only (no valid crosshair target at open)
			std::string  targetName;
		};
		PendingWheel g_wheel;

		// Crosshair target captured at browser-open time. The engine clears the reticle slot
		// (PlayerCharacter+0xF90) to null while ANY menu is up (OSF RE gameplay.crosshair_pick),
		// so a PICK clicked inside the open browser can never read it live — it resolves this
		// capture instead. 0 = nothing was under the reticle at open. Cleared on osf.closed.
		// Game main thread only, like g_tokens.
		std::int32_t g_openPickToken = 0;

		// token -> picked ref. All handlers run on the GAME MAIN THREAD (CommandFn contract), and the token map is only ever touched from a handler, so no locking is needed. 
		// token -1 is reserved for the player (never stored).
		struct Picked
		{
			RE::TESObjectREFR* ref;     // the pointer resolved AT PICK TIME (main thread)
			RE::TESFormID      formID;  // re-validated at use: LookupByID must still == ref
			bool               isActor;
		};
		std::unordered_map<std::int32_t, Picked> g_tokens;
		std::int32_t                             g_nextToken = 1;
		// formID -> token, so a re-scan / re-pick of the same ref reuses its token instead of growing the table without bound.
		std::unordered_map<RE::TESFormID, std::int32_t> g_formToken;

		// Mint (or reuse) a token for a ref and record it. Main thread only.
		std::int32_t AllocToken(RE::TESObjectREFR* a_ref)
		{
			const RE::TESFormID fid = a_ref->GetFormID();
			if (const auto it = g_formToken.find(fid); it != g_formToken.end()) {
				g_tokens[it->second] = Picked{ a_ref, fid, a_ref->IsActor() };  // refresh the pointer
				return it->second;
			}
			const std::int32_t token = g_nextToken++;
			g_tokens[token] = Picked{ a_ref, fid, a_ref->IsActor() };
			g_formToken[fid] = token;
			return token;
		}

		// "AnimFurnChairScrappy" -> "Chair Scrappy": strip the AnimFurn prefix, split CamelCase.
		// The anchor keyword is the only human-readable runtime name an invisible AI marker has
		// (FURN base forms don't retain editor IDs; keywords do), and it names what the spot hosts.
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

		// A human label for a scanned ref. Invisible AI markers and outpost/dynamic furniture
		// return an empty display name, so fall back to the matched anchor keyword, then the base
		// object's EditorID, then a form-id tag, so a pick is never a bare "(unnamed)" the user
		// cannot identify.
		std::string ScanLabel(RE::TESObjectREFR* a_ref, RE::BGSKeyword* a_matchedKw = nullptr)
		{
			if (const char* nm = a_ref->GetDisplayFullName(); nm && nm[0]) {
				return nm;
			}
			if (std::string kwLabel = KeywordLabel(a_matchedKw); !kwLabel.empty()) {
				return kwLabel;
			}
			if (const auto base = a_ref->GetBaseObject()) {
				if (const char* edid = base->GetFormEditorID(); edid && edid[0]) {
					return edid;
				}
				return std::format("Furniture {:#010x}", base->GetFormID());
			}
			return std::format("Ref {:#010x}", a_ref->GetFormID());
		}

		// Our view's manifest id; the SendToWeb target for pushes that aren't a direct reply (e.g. the catalog we push when the bridge becomes ready).
		constexpr const char* kViewId = "osf.animation/browser";  // qualified "<modId>/<viewName>" (OSF UI api-freeze item 1)

		// The OSF UI release this build was developed and tested against. When the
		// installed host reports an older version, the browser's status line grows an
		// UPDATE badge pointing at the OSF UI Nexus page. Bump alongside any new
		// host feature this file starts depending on.
		// OSF UI 1.5 is the first release whose consented reporter recognizes
		// osf.animation/* as the OSF Animation target, attaches this plugin's
		// session log, and routes the resulting issue to the Animation repo.
		constexpr std::uint32_t kOSFUITested[3] = { 1, 5, 0 };
		constexpr const char*   kOSFUINexusURL  = "https://www.nexusmods.com/starfield/mods/17711";

		// ---- helpers ---------------------------------------------------------

		IOSFSceneAPI* SceneAPI()
		{
			if (!g_scene) {
				g_scene = OSF_RequestSceneAPI(kOSFSceneAPIVersion);
			}
			return g_scene;
		}

		// Serialize a payload to the source view. Uses the replace error handler so as non-UTF-8 game name can never throw out of a noexcept handler.
		void SendJson(const char* a_view, const char* a_type, const json& a_payload)
		{
			if (!g_ui) {
				return;
			}
			const std::string text = a_payload.dump(-1, ' ', false, json::error_handler_t::replace);
			g_ui.SendToWeb(a_view, a_type, text.c_str());
		}

		// Parse an inbound payload without throwing (handlers are noexcept). Returns a discarded value on malformed input; callers treat non-objects as empty.
		json ParsePayload(const char* a_json)
		{
			return json::parse(a_json ? a_json : "", nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
		}

		// Every live scene for the view's ACTIVE list: handle, sceneId, stage, whether the
		// player is in the cast, and the cast itself (token + name; the player is token -1,
		// NPCs get pick tokens so the view can badge busy crew members and re-target stops).
		// Main thread only (AllocToken touches g_tokens).
		json BuildActiveScenes()
		{
			auto& rt = Scene::SceneRuntime::GetSingleton();
			auto* player = RE::PlayerCharacter::GetSingleton();
			json scenes = json::array();
			for (const auto& s : rt.ListScenes()) {
				json cast = json::array();
				bool hasPlayer = false;
				for (RE::Actor* a : s.participants) {
					if (!a) {
						continue;
					}
					const bool isPlayer = player && a == static_cast<RE::Actor*>(player);
					hasPlayer = hasPlayer || isPlayer;
					cast.push_back(json{
						{ "token", isPlayer ? -1 : AllocToken(a) },
						{ "name", isPlayer ? std::string{ "Player" } : ScanLabel(a) },
						{ "player", isPlayer },
					});
				}
				scenes.push_back(json{
					{ "handle", s.handle },
					{ "sceneId", s.id.empty() ? std::string{ "runtime.files" } : s.id },
					{ "stage", rt.GetStage(s.handle) },
					{ "player", hasPlayer },
					{ "cast", std::move(cast) },
				});
			}
			return json{ { "scenes", std::move(scenes) } };
		}

		// Push the live-scene list to the view. Dropped while the browser is hidden — the
		// osf.opened handler re-sends, so a reopen always shows the current list (including
		// NPC scenes still running from an earlier session). Main thread only.
		void PushActiveScenes()
		{
			if (!g_ui || !g_uiReady || !g_viewVisible) {
				return;
			}
			SendJson(kViewId, "osf.animation.activeScenes", BuildActiveScenes());
		}

		// The player's crosshair/reticle target as a validated object reference, or nullptr.
		// crosshairRef (+0xF90) is runtime-proven on 1.16.244 (OSF RE gameplay.crosshair_pick;
		// F11 probe 2026-07-18) and pinned by an offsetof assert in the CLSF header — the old
		// `commandTarget` member compiled +0x48 late onto the CELL slot. The engine nulls the
		// slot while any menu is up. Main thread only.
		RE::TESObjectREFR* CrosshairRef()
		{
			auto*              player = RE::PlayerCharacter::GetSingleton();
			RE::TESObjectREFR* ref = player ? player->crosshairRef : nullptr;
			return (ref && (ref->Is(RE::FormType::kREFR) || ref->Is(RE::FormType::kACHR))) ? ref : nullptr;
		}

		// Deliver the wheel-mode switch to the view. target:null = the wheel plays on the player.
		// Idempotent on the view side, so the OpenWheel send and the osf.opened replay can both land.
		void SendWheelMode()
		{
			json payload = { { "mode", "wheel" }, { "tagPrefix", g_wheel.tagPrefix } };
			if (g_wheel.targetToken != 0) {
				payload["target"] = { { "token", g_wheel.targetToken }, { "name", g_wheel.targetName } };
			} else {
				payload["target"] = nullptr;
			}
			SendJson(kViewId, "osf.animation.mode", payload);
		}

		// Actor sex as the view's M/F badge tag: "male" / "female", "" for furniture, creatures and
		// any actor with no actorbase sex. Same tag the matchmaker binds gendered role slots with.
		std::string RefSexTag(RE::TESObjectREFR* a_ref)
		{
			return a_ref && a_ref->IsActor()
			         ? Matchmaking::ActorGenderTag(static_cast<RE::Actor*>(a_ref))
			         : std::string{};
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

		// Re-resolve a token to a still-live ref on the main thread. token -1 = player. Guards against unload / formID reuse: the id must still resolve to the very same form we stored, and it must not be flagged deleted.
		RE::TESObjectREFR* ResolveToken(std::int32_t a_token)
		{
			if (a_token == -1) {
				return RE::PlayerCharacter::GetSingleton();
			}
			const auto it = g_tokens.find(a_token);
			if (it == g_tokens.end()) {
				return nullptr;
			}
			const Picked& p = it->second;
			RE::TESForm* form = RE::TESForm::LookupByID(p.formID);
			if (!form || form != static_cast<RE::TESForm*>(p.ref) || form->IsDeleted()) {
				return nullptr;  // gone, reused, or deleted since it was picked
			}
			return p.ref;
		}

		// opts tri-state: true/1 -> 1 (on), false/0 -> 0 (off), anything else -> -1 (inherit).
		std::int32_t OptTri(const json& a_opts, const char* a_key)
		{
			if (!a_opts.is_object()) {
				return -1;
			}
			const auto it = a_opts.find(a_key);
			if (it == a_opts.end()) {
				return -1;
			}
			if (it->is_boolean()) {
				return it->get<bool>() ? 1 : 0;
			}
			if (it->is_number_integer()) {
				const std::int32_t v = it->get<std::int32_t>();
				return (v == 0 || v == 1) ? v : -1;
			}
			return -1;
		}

		// A human-readable reason a launch returned handle 0, best-effort.
		std::string LaunchError(const std::string& a_sceneId, std::size_t a_castCount, bool a_haveFurniture)
		{
			auto&       reg = Registry::SceneRegistry::GetSingleton();
			const auto def = reg.Find(a_sceneId);
			if (!def) {
				return "Unknown scene '" + a_sceneId + "'";
			}
			if (def->RequiresAnchor() && !a_haveFurniture) {
				return "This scene needs furniture — pick a furniture target first";
			}
			if (a_castCount == 0) {
				return "No cast selected";
			}
			// Surface any load-time diagnostics that name this scene.
			const std::string idLower = Util::ToLower(a_sceneId);
			std::string       joined;
			for (const auto& e : reg.LoadErrors()) {
				if (Util::ToLower(e).find(idLower) != std::string::npos) {
					if (!joined.empty()) {
						joined += "; ";
					}
					joined += e;
				}
			}
			if (!joined.empty()) {
				return joined;
			}
			return "Scene failed to start — a cast member may already be in a scene, or a required clip is missing";
		}

		// ---- command handlers (GAME MAIN THREAD) -----------------------------

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
				std::string              sig;    // clip-set signature (files joined) for de-dup
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
				bool                     requiresFurniture = false;
				bool                     inPlace = false;
				std::vector<std::string> anchorNames;  // human labels for WHAT the scene anchors to ("Barstool", ...)
				bool                     unlisted = false;
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
				for (const auto& r : d.roles) {
					c.genders.emplace_back(GenderTag(r.gender));
				}
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
					for (const auto& clip : st.clips) {
						sc.sig += clip.file;
						sc.sig += '\n';
					}

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
						{ "sig", s.sig },
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
					{ "requiresFurniture", c.requiresFurniture },
					{ "inPlace", c.inPlace },
					{ "anchors", c.anchorNames },
					{ "unlisted", c.unlisted },
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

		// The host segment of the identity payload: OSF UI's installed version, plus the
		// update verdict against kOSFUITested (the view can't compare versions it doesn't
		// know about, and the Nexus URL is plugin knowledge, not view knowledge).
		json UIHostInfo() noexcept
		{
			std::uint32_t mj = 0, mn = 0, pt = 0;
			g_ui.GetPluginVersion(mj, mn, pt);
			if (mj == 0 && mn == 0 && pt == 0) {
				return nullptr;  // host absent or not reporting — the view omits the segment
			}
			const bool outdated =
				std::tie(mj, mn, pt) < std::tie(kOSFUITested[0], kOSFUITested[1], kOSFUITested[2]);
			return json{
				{ "name", "OSF UI" },
				{ "version", std::format("{}.{}.{}", mj, mn, pt) },
				{ "tested", std::format("{}.{}.{}", kOSFUITested[0], kOSFUITested[1], kOSFUITested[2]) },
				{ "outdated", outdated },
				{ "nexusUrl", kOSFUINexusURL },
			};
		}

		void OnCatalogGet(const char*, const char*, const char* a_srcView, void*) noexcept
		{
			// The view's status line should name THIS plugin — runtime.ready carries the
			// OSF UI host's identity, not ours. Piggyback on the catalog request the view
			// always makes right after ready (and on every refresh). `ui` describes the
			// HOST (installed OSF UI version + update verdict) so the browser can surface
			// an outdated host without knowing version history itself.
			SendJson(a_srcView, "osf.animation.version", json{
				{ "plugin", SFSE::GetPluginName() },
				// major.minor.patch only — Version::string() would append the unused build
				// field and render as "1.0.0.0" in the view's status line.
				{ "version", std::format("{}.{}.{}", SFSE::GetPluginVersion().major(), SFSE::GetPluginVersion().minor(), SFSE::GetPluginVersion().patch()) },
				{ "ui", UIHostInfo() },
				// The player is a permanent crew member the view never scans, so its M/F badge
				// has no other channel — ride along with the identity push.
				{ "playerSex", RefSexTag(RE::PlayerCharacter::GetSingleton()) },
			});
			SendJson(a_srcView, "osf.animation.catalog.data", BuildCatalog(false));
		}

		// The library lane is static after load (generated packs, pack-authored durations), so the
		// view fetches it once on demand and caches — it is never re-pushed by catalog updates.
		void OnLibraryGet(const char*, const char*, const char* a_srcView, void*) noexcept
		{
			SendJson(a_srcView, "osf.animation.library.data", BuildCatalog(true));
		}

		void OnPickCrosshair(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const json j = ParsePayload(a_payload);
			std::string slot = "actor";
			if (j.is_object()) {
				if (const auto it = j.find("slot"); it != j.end() && it->is_string()) {
					slot = it->get<std::string>();
				}
			}
			const bool wantActor = (slot != "furniture");

			// Live read first (covers any host that leaves the reticle active), but with the
			// browser menu up the engine has cleared the slot — fall back to the target that
			// was under the crosshair when the browser opened (re-validated via the token map).
			RE::TESObjectREFR* ref = CrosshairRef();
			if (!ref && g_openPickToken != 0) {
				ref = ResolveToken(g_openPickToken);
			}
			const bool accept = ref && (!wantActor || ref->IsActor());

			json reply;
			reply["slot"] = slot;
			if (!accept) {
				reply["valid"] = false;
				reply["token"] = 0;
				reply["name"] = "";
				reply["formId"] = 0;
				REX::DEBUG("[UI] osf.animation.pickCrosshair slot={} -> nothing valid (live null, openToken={})", slot, g_openPickToken);
			} else {
				const std::int32_t token = AllocToken(ref);
				const std::string  nm = ScanLabel(ref);
				reply["valid"] = true;
				reply["token"] = token;
				reply["name"] = nm;
				reply["formId"] = ref->GetFormID();
				// Skeleton family, so the view can filter the library to what this actor can actually
				// play (a creature gets its own animations, not human ones). "" for furniture picks.
				reply["species"] = ref->IsActor() ? Util::ActorSpecies(static_cast<RE::Actor*>(ref)) : std::string{};
				reply["sex"] = RefSexTag(ref);
				REX::DEBUG("[UI] osf.animation.pickCrosshair slot={} -> token {} '{}' ({:08X})", slot, token, nm, ref->GetFormID());
			}
			SendJson(a_srcView, "osf.animation.pick", reply);
		}

		struct SafeViewProjection
		{
			// Keep the camera graph alive while a command projects all requested points.
			RE::NiPointer<RE::NiNode> root;
			RE::NiCamera*             camera{ nullptr };
			RE::NiPoint3              position;
			RE::NiPoint3              forward;
			RE::NiPoint3              right;
			RE::NiPoint3              up;

			bool Project(const RE::NiPoint3& a_world, RE::NiPoint3& a_screen) const
			{
				if (!camera) {
					return false;
				}
				const RE::NiPoint3 delta = a_world - position;
				const float depth = delta.Dot(forward);
				if (!std::isfinite(depth) || depth <= 0.01f) {
					return false;
				}

				// NiCamera owns the exact per-frame projection used for culling, including the
				// current FOV, aspect ratio and viewport. A hand-built fixed-FOV projection drifts
				// as soon as the game or scene camera changes its lens.
				a_screen = camera->WorldToScreenNormalized(a_world);
				a_screen.z = depth;
				return std::isfinite(a_screen.x) && std::isfinite(a_screen.y) &&
				       std::isfinite(a_screen.z);
			}

			float ProjectedRadius(const RE::NiPoint3& a_center, float a_radius,
				float a_width, float a_height) const
			{
				RE::NiPoint3 center;
				RE::NiPoint3 edgeX;
				RE::NiPoint3 edgeY;
				if (!Project(a_center, center) ||
					!Project(a_center + right * a_radius, edgeX) ||
					!Project(a_center + up * a_radius, edgeY)) {
					return 0.0f;
				}
				return std::max(std::abs(edgeX.x - center.x) * a_width,
					std::abs(edgeY.y - center.y) * a_height);
			}
		};

		RE::NiCamera* FindCameraInNode(RE::NiAVObject* a_object, std::uint32_t a_depth = 0)
		{
			if (!a_object || a_depth > 16) {
				return nullptr;
			}
			if (auto* camera = starfield_cast<RE::NiCamera*>(a_object)) {
				return camera;
			}
			auto* node = starfield_cast<RE::NiNode*>(a_object);
			if (!node) {
				return nullptr;
			}
			for (const auto& child : node->children) {
				if (auto* camera = FindCameraInNode(child.get(), a_depth + 1)) {
					return camera;
				}
			}
			return nullptr;
		}

		RE::NiCamera* ActiveWorldCamera()
		{
			// RUNTIME-PROVEN on 1.16.244: Address Library ID 936470 is the global
			// StorageTable::Camera host-memory pointer. Its inline NiCamera at +0x80 is
			// camera B, the main WORLD render camera. PlayerCamera::cameraRoot reaches
			// only camera A (the gameplay/viewmodel camera), which is wrong in third
			// person and scene orbit.
			static const REL::Relocation<std::uintptr_t> storageGlobal{ REL::ID(936470) };
			static const REL::Relocation<std::uintptr_t> cameraVtable{ RE::NiCamera::VTABLE[0] };

			const auto storage = *reinterpret_cast<const std::uintptr_t*>(storageGlobal.address());
			if (storage != 0) {
				auto* candidate = reinterpret_cast<RE::NiCamera*>(storage + 0x80);
				if (*reinterpret_cast<const std::uintptr_t*>(candidate) == cameraVtable.address()) {
					return candidate;
				}
			}

			// Defensive fallback for a future runtime whose renderer storage layout moves:
			// a camera nested under the mapped PlayerCamera root is still preferable to
			// dropping every indicator, although it may represent the viewmodel lens.
			auto* playerCamera = RE::PlayerCamera::GetSingleton();
			const auto root = playerCamera ? playerCamera->cameraRoot : nullptr;
			return root ? FindCameraInNode(root.get()) : nullptr;
		}

		// Rate limiter for camera-anomaly logs: picking polls at ~10 Hz, so an unhealthy
		// camera would otherwise repeat the same line for as long as it stays unhealthy.
		bool ShouldLogCameraAnomaly()
		{
			static std::chrono::steady_clock::time_point s_last{};
			const auto now = std::chrono::steady_clock::now();
			if (now - s_last < std::chrono::seconds(2)) {
				return false;
			}
			s_last = now;
			return true;
		}

		// PlayerCamera::cameraRoot is a mapped, runtime-proven field already used by the
		// camera service. It supplies a lifetime pin and a fallback; ActiveWorldCamera()
		// resolves the separate main-world renderer camera used for exact projection.
		//
		// Two health checks guard the result, because the reported failure mode of world
		// picking was not "misses by a bit" but "markers and clicks land nowhere near the
		// visible world, until further notice":
		//   1. While OSF's scene orbit drives the camera (the browser is open — exactly when
		//      picking runs), the orbit pose is the one view pose OSF computes itself, so it
		//      can't go stale. A resolved camera sitting far from that pose is NOT the camera
		//      the world is rendered through — prefer whichever known camera is at the pose.
		//   2. worldToCam (what Project uses) is a CPU-side matrix rebuilt asynchronously from
		//      the camera's world transform (see NiCamera.h); a probe point straight down the
		//      camera's own forward must project to the viewport center. When matrix and
		//      transform disagree beyond one frame of skew, fail the whole query: no markers
		//      for that beat (plus a log saying why) beats markers that lie — and since a
		//      click resolves against the marker the user saw, a click never projects at all.
		std::optional<SafeViewProjection> CurrentViewProjection(float a_width, float a_height)
		{
			auto* playerCamera = RE::PlayerCamera::GetSingleton();
			const auto root = playerCamera ? playerCamera->cameraRoot : nullptr;
			if (!root || !std::isfinite(a_width) || !std::isfinite(a_height) || a_width < 1.0f || a_height < 1.0f) {
				return std::nullopt;
			}

			SafeViewProjection out;
			out.root = root;
			out.camera = ActiveWorldCamera();

			float orbitPos[3];
			float orbitFwd[3];
			if (Camera::CameraService::GetSingleton().SceneOrbitPose(orbitPos, orbitFwd)) {
				const auto poseErrorSq = [&](RE::NiCamera* a_camera) {
					const float dx = a_camera->world.translate.x - orbitPos[0];
					const float dy = a_camera->world.translate.y - orbitPos[1];
					const float dz = a_camera->world.translate.z - orbitPos[2];
					return dx * dx + dy * dy + dz * dz;
				};
				constexpr float kPoseToleranceSq = 1.5f * 1.5f;  // generous: covers the orbit glide's frame lag
				RE::NiCamera*   alt = FindCameraInNode(root.get());
				const float     mainErr = out.camera ? poseErrorSq(out.camera) : std::numeric_limits<float>::max();
				const float     altErr = (alt && alt != out.camera) ? poseErrorSq(alt) : std::numeric_limits<float>::max();
				if (mainErr > kPoseToleranceSq) {
					if (altErr < mainErr) {
						if (ShouldLogCameraAnomaly()) {
							REX::WARN("[UI] world-pick camera: storage camera sits {:.1f} from the live orbit pose — projecting through the cameraRoot camera instead ({:.1f})",
								std::sqrt(mainErr), std::sqrt(altErr));
						}
						out.camera = alt;
					} else if (ShouldLogCameraAnomaly()) {
						REX::WARN("[UI] world-pick camera sits {:.1f} from the live orbit pose — picking may not line up", std::sqrt(mainErr));
					}
				}
			}
			if (!out.camera) {
				return std::nullopt;
			}
			out.position = out.camera->world.translate;
			out.forward = {
				out.camera->world.rotate[0][0],
				out.camera->world.rotate[0][1],
				out.camera->world.rotate[0][2],
			};
			if (out.forward.Unitize() <= 0.001f) {
				return std::nullopt;
			}
			out.right = out.forward.Cross(RE::NiPoint3{ 0.0f, 0.0f, 1.0f });
			if (out.right.Unitize() <= 0.001f) {
				return std::nullopt;
			}
			out.up = out.right.Cross(out.forward);
			if (out.up.Unitize() <= 0.001f) {
				return std::nullopt;
			}

			// Health check 2 (see above): the camera's own forward axis must project to the
			// viewport center. 0.12 normalized is loose enough to pass one frame of update
			// skew during a violent orbit flick, and tight enough to catch a frozen matrix.
			// A second probe offset to the right must land measurably off the first — a
			// degenerate matrix collapses WorldToScreen to its (0,0) sentinel, which
			// normalizes to exactly (0.5, 0.5) and would sail through the center check.
			RE::NiPoint3 probe{ -1.0f, -1.0f, -1.0f };
			RE::NiPoint3 probeRight{ -1.0f, -1.0f, -1.0f };
			if (!out.Project(out.position + out.forward * 10.0f, probe) ||
				std::abs(probe.x - 0.5f) > 0.12f || std::abs(probe.y - 0.5f) > 0.12f ||
				!out.Project(out.position + out.forward * 10.0f + out.right, probeRight) ||
				std::abs(probeRight.x - probe.x) < 0.005f) {
				if (ShouldLogCameraAnomaly()) {
					REX::WARN("[UI] world-pick projection rejected: camera matrix disagrees with its own transform (forward probe {:.2f},{:.2f}; right offset {:.3f})",
						probe.x, probe.y, std::abs(probeRight.x - probe.x));
				}
				return std::nullopt;
			}
			return out;
		}

		bool RenderedBound(RE::TESObjectREFR* a_ref, bool a_actor, RE::NiPoint3& a_center, float& a_radius)
		{
			if (!a_ref || a_ref->IsDeleted()) {
				return false;
			}
			RE::NiPointer<RE::NiAVObject> node;
			{
				const auto loaded = a_ref->loadedData.LockRead();
				if (*loaded) {
					node = (*loaded)->data3D;
				}
			}
			if (!node) {
				return false;
			}
			a_center = node->worldBound.center;
			a_radius = node->worldBound.radius;
			if (!std::isfinite(a_radius) || a_radius < 0.01f || a_radius > 10000.0f ||
				!std::isfinite(a_center.x) || !std::isfinite(a_center.y) || !std::isfinite(a_center.z)) {
				a_center = node->world.translate;
				a_radius = a_actor ? 0.8f : 0.6f;
			}
			return true;
		}

		bool RenderedActorLabelPoint(RE::TESObjectREFR* a_ref, RE::NiPoint3& a_point)
		{
			if (!a_ref || !a_ref->IsActor() || a_ref->IsDeleted()) {
				return false;
			}
			RE::NiPointer<RE::NiAVObject> root;
			{
				const auto loaded = a_ref->loadedData.LockRead();
				if (*loaded) {
					root = (*loaded)->data3D;
				}
			}
			if (!root) {
				return false;
			}

			// The label belongs to the rendered head, not the actor's worldBound. worldBound is
			// a culling sphere and may be expanded or re-centered by weapons, animation and OSF's
			// compose-root cull pin, so center+radius is not a stable anatomical point.
			static const RE::BSFixedString headName{ "C_Head" };
			if (RE::NiAVObject* head = root->GetObjectByName(headName)) {
				// Render-node transforms are in meters (unlike TESObjectREFR logical
				// positions). Twelve centimetres clears the top of the rendered head.
				a_point = head->world.translate + RE::NiPoint3{ 0.0f, 0.0f, 0.12f };
				return std::isfinite(a_point.x) && std::isfinite(a_point.y) && std::isfinite(a_point.z);
			}

			// Creature rigs do not consistently expose C_Head. Keep their fallback close to the
			// rendered body by clamping the culling radius to plausible dimensions in METERS.
			const RE::NiPoint3 center = root->worldBound.center;
			const float radius = std::clamp(root->worldBound.radius, 0.45f, 1.35f);
			a_point = center + RE::NiPoint3{ 0.0f, 0.0f, radius };
			return std::isfinite(a_point.x) && std::isfinite(a_point.y) && std::isfinite(a_point.z);
		}

		bool RenderedFurnitureLabelPoint(RE::TESObjectREFR* a_ref, RE::NiPoint3& a_point)
		{
			const auto base = a_ref ? a_ref->GetBaseObject() : nullptr;
			if (!a_ref || a_ref->IsDeleted() || !base || !base->Is(RE::FormType::kFURN)) {
				return false;
			}

			RE::NiPoint3 center;
			float        radius = 0.0f;
			if (!RenderedBound(a_ref, false, center, radius)) {
				return false;
			}
			// Float the label above the rendered object. Clamp the culling radius so
			// oversized workbenches and tiny/invisible idle markers stay readable.
			a_point = center + RE::NiPoint3{ 0.0f, 0.0f, std::clamp(radius, 0.25f, 1.4f) };
			return std::isfinite(a_point.x) && std::isfinite(a_point.y) && std::isfinite(a_point.z);
		}

		void OnProjectActors(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const json j = ParsePayload(a_payload);
			json items = json::array();
			if (!j.is_object() || !j.contains("tokens") || !j["tokens"].is_array()) {
				SendJson(a_srcView, "osf.animation.actorIndicators", json{ { "items", std::move(items) } });
				return;
			}
			const float width = std::clamp(j.value("width", 1280.0f), 320.0f, 10000.0f);
			const float height = std::clamp(j.value("height", 720.0f), 200.0f, 10000.0f);
			const auto projection = CurrentViewProjection(width, height);
			if (!projection) {
				SendJson(a_srcView, "osf.animation.actorIndicators", json{ { "items", std::move(items) } });
				return;
			}

			constexpr std::size_t kMaxIndicators = 16;
			for (const auto& value : j["tokens"]) {
				if (items.size() >= kMaxIndicators || !value.is_number_integer()) {
					break;
				}
				const std::int32_t token = value.get<std::int32_t>();
				RE::TESObjectREFR* ref = ResolveToken(token);
				if (!ref) {
					continue;
				}
				RE::NiPoint3 labelPoint;
				RE::NiPoint3 screen;
				const bool hasLabelPoint = ref->IsActor()
				                               ? RenderedActorLabelPoint(ref, labelPoint)
				                               : RenderedFurnitureLabelPoint(ref, labelPoint);
				const bool projected = hasLabelPoint &&
					projection->Project(labelPoint, screen);
				const bool visible = projected && screen.x >= 0.0f && screen.x <= 1.0f && screen.y >= 0.0f && screen.y <= 1.0f;
				items.push_back(json{
					{ "token", token },
					{ "x", projected ? screen.x : 0.0f },
					{ "y", projected ? screen.y : 0.0f },
					{ "visible", visible },
				});
			}
			SendJson(a_srcView, "osf.animation.actorIndicators", json{ { "items", std::move(items) } });
		}
		void OnLaunch(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const json j = ParsePayload(a_payload);
			json       reply;

			const std::string sceneId = (j.is_object() && j.contains("sceneId") && j["sceneId"].is_string())
			                                ? j["sceneId"].get<std::string>()
			                                : std::string{};
			reply["sceneId"] = sceneId;
			// A browser animation row references one stage inside a registry-backed
			// collection. Its launch must end when that stage exits instead of walking
			// the collection's remaining stage chain. The wheel implies the same scope.
			const bool singleAnimation = j.is_object() && j.value("singleAnimation", false);

			auto fail = [&](const std::string& a_reason) {
				reply["ok"] = false;
				reply["handle"] = 0;
				reply["error"] = a_reason;
				REX::WARN("[UI] osf.animation.launch '{}' refused: {}", sceneId, a_reason);
				SendJson(a_srcView, "osf.animation.launchResult", reply);
			};

			if (sceneId.empty()) {
				return fail("No scene selected");
			}

			// Resolve the cast tokens back to live actors (main thread).
			std::vector<RE::Actor*> actors;
			if (j.contains("castTokens") && j["castTokens"].is_array()) {
				for (const auto& t : j["castTokens"]) {
					if (!t.is_number_integer()) {
						return fail("Malformed cast token");
					}
					RE::TESObjectREFR* r = ResolveToken(t.get<std::int32_t>());
					if (!r || !r->IsActor()) {
						return fail("A selected cast member is no longer available — re-pick it");
					}
					actors.push_back(static_cast<RE::Actor*>(r));
				}
			}
			if (actors.empty()) {
				return fail("No cast selected");
			}

			// Optional furniture anchor.
			RE::TESObjectREFR* furniture = nullptr;
			if (j.contains("furnitureToken") && j["furnitureToken"].is_number_integer()) {
				const std::int32_t ftok = j["furnitureToken"].get<std::int32_t>();
				if (ftok != 0) {
					furniture = ResolveToken(ftok);
					if (!furniture) {
						return fail("The furniture target is no longer available — re-pick it");
					}
				}
			}

			// Build the per-start options POD from the minimal opts block.
			json opts = json::object();
			if (j.is_object()) {
				if (const auto it = j.find("opts"); it != j.end() && it->is_object()) {
					opts = *it;
				}
			}
			OSFStartOptions o{};
			o.stripMode = OptTri(opts, "strip");
			o.lockPlayerMode = OptTri(opts, "lockPlayer");
			o.playerControlMode = OptTri(opts, "playerControl");
			o.fadeMode = OptTri(opts, "fade");
			o.speed = opts.value("speed", 1.0f);
			// Enter the scene on a specific linear stage. 0 = the scene's entry; resolved to the stage's
			// node BEFORE the start (ResolveStartStageNode), so the scene opens directly on it.
			o.startStage = opts.value("stage", 0);
			if (const auto it = opts.find("camera"); it != opts.end() && it->is_string()) {
				std::snprintf(o.camera, sizeof(o.camera), "%s", it->get<std::string>().c_str());
			}
			o.anchorRef = furniture;
			// Browser location selector. This is deliberately a bridge-only extension: the stable native
			// OSFStartOptions already represents both reference and explicit-world anchors.
			std::string locationMode = furniture ? "furniture" : "cast";
			std::int32_t locationToken = 0;
			if (j.is_object()) {
				if (const auto it = j.find("location"); it != j.end() && it->is_object()) {
					if (const auto mode = it->find("mode"); mode != it->end() && mode->is_string()) {
						locationMode = mode->get<std::string>();
					}
					if (const auto token = it->find("token"); token != it->end() && token->is_number_integer()) {
						locationToken = token->get<std::int32_t>();
					}
				}
			}

			if (locationMode == "player") {
				o.anchorRef = RE::PlayerCharacter::GetSingleton();
				if (!o.anchorRef) {
					return fail("The player is not available as a scene location");
				}
			} else if (locationMode == "actor") {
				o.anchorRef = ResolveToken(locationToken);
				if (!o.anchorRef || !o.anchorRef->IsActor()) {
					return fail("The selected actor location is no longer available — re-pick it");
				}
			} else if (locationMode == "furniture") {
				if (locationToken != 0) {
					o.anchorRef = ResolveToken(locationToken);
				}
				if (o.anchorRef && o.anchorRef->IsActor()) {
					return fail("The selected furniture location is an actor — pick furniture or a marker");
				}
				furniture = o.anchorRef;
			} else if (locationMode == "front") {
				// Starfield's NiPoint3 world transforms are meters. Ten feet = 3.048 m.
				const auto anchor = Scene::MakeAnchorInFrontOfView(RE::PlayerCharacter::GetSingleton(), 3.048f);
				if (!anchor.set) {
					return fail("The player is not available for front-of-player placement");
				}
				o.anchorRef = nullptr;
				o.hasAnchor = true;
				o.anchorX = anchor.pos.x;
				o.anchorY = anchor.pos.y;
				o.anchorZ = anchor.pos.z;
				o.anchorHeadingRad = anchor.heading;
			} else if (locationMode != "cast") {
				return fail("Unknown scene location mode '" + locationMode + "'");
			}

			std::string castDiag;
			for (RE::Actor* actor : actors) {
				castDiag += std::format("{}{:08X}", castDiag.empty() ? "" : ",", actor->formID);
			}
			REX::DEBUG("[UI] launch request '{}' cast=[{}] location={} token={} activeBefore={}",
				sceneId, castDiag, locationMode, locationToken, Scene::SceneRuntime::GetSingleton().ListScenes().size());

			// WHEEL POSTURE: every wheel launch is a quick in-world flourish and gets the same
			// hands-off settings regardless of which pack the animation came from — play it on the
			// actor where they stand (no teleport / per-frame root+heading pin), never touch the
			// camera (vanilla third person stays live; the pin fight was the emote camera judder),
			// keep the player's controls, no strip, no fade — and SINGLE-ANIMATION: the scene is
			// pinned to the picked stage after launch (SetSingleStage below), so space cancels the
			// emote instead of cycling the pack's stages and the pack's own stage chain can't step
			// past it. Enforced HERE (not per-pack policy) so stage-pinned library animations
			// behave exactly like the installed emotes.
			if (g_wheel.active) {
				o.inPlaceMode = 1;
				o.lockPlayerMode = 0;
				o.stripMode = 0;
				o.fadeMode = 0;
				std::snprintf(o.camera, sizeof(o.camera), "none");
			}

			auto* api = SceneAPI();
			if (!api) {
				return fail("OSF Animation engine is not ready yet");
			}

			// Replace-in-place: if a cast member is already mid-scene, stop that scene first so this launch supersedes it 
			for (RE::Actor* a : actors) {
				const std::int32_t busy = api->GetSceneForActor(a);
				if (busy != 0) {
					api->StopScene(busy);
					std::erase(g_closeStops, busy);
					if (busy == g_lastHandle) {
						g_lastHandle = 0;
					}
					REX::INFO("[UI] osf.animation.launch '{}' superseding live scene {:#010x} (cast busy) — stopped it first", sceneId, busy);
				}
			}

			// Named-role binding if the view supplied roleNames (one per cast token); else order-based auto-bind.
			std::int32_t handle = 0;
			std::vector<std::string> roleNames;
			if (j.contains("roleNames") && j["roleNames"].is_array()) {
				for (const auto& r : j["roleNames"]) {
					roleNames.push_back(r.is_string() ? r.get<std::string>() : std::string{});
				}
			}
			if (!roleNames.empty() && roleNames.size() == actors.size()) {
				std::vector<const char*> rolePtrs;
				rolePtrs.reserve(roleNames.size());
				for (const auto& r : roleNames) {
					rolePtrs.push_back(r.c_str());
				}
				handle = api->StartSceneRoles(actors.data(), static_cast<std::uint32_t>(actors.size()),
					sceneId.c_str(), rolePtrs.data(), static_cast<std::uint32_t>(rolePtrs.size()), o);
			} else {
				handle = api->StartScene(actors.data(), static_cast<std::uint32_t>(actors.size()), sceneId.c_str(), o);
			}

			if (handle == 0) {
				return fail(LaunchError(sceneId, actors.size(), furniture != nullptr));
			}

			g_lastHandle = handle;
			// A stage-scoped browser item and a wheel entry both mean "play this
			// animation", never "start here and continue through the parent collection."
			// Post-start on purpose: this is launch posture, not authored scene policy.
			if (g_wheel.active || singleAnimation) {
				Scene::SceneRuntime::GetSingleton().SetSingleStage(handle);
			}
			// Console launch with the PLAYER in the cast: abort on browser close (see
			// g_closeStops). NPC-only casts and wheel emotes outlive the close.
			auto* player = RE::PlayerCharacter::GetSingleton();
			const bool castHasPlayer = player &&
				std::find(actors.begin(), actors.end(), static_cast<RE::Actor*>(player)) != actors.end();
			if (!g_wheel.active && castHasPlayer) {
				g_closeStops.push_back(handle);
			}
			reply["ok"] = true;
			reply["handle"] = handle;
			reply["autoMinimize"] = g_browserAutoMinimize;
			REX::INFO("[UI] osf.animation.launch '{}' -> handle {} ({} cast{}{})", sceneId, handle, actors.size(),
				furniture ? ", anchored" : "", castHasPlayer ? "" : ", NPC-only — outlives the browser");
			SendJson(a_srcView, "osf.animation.launchResult", reply);
			PushActiveScenes();
		}

		void OnStop(const char*, const char* a_payload, const char*, void*) noexcept
		{
			const json   j = ParsePayload(a_payload);
			std::int32_t handle = 0;
			if (j.is_object()) {
				if (const auto it = j.find("handle"); it != j.end() && it->is_number_integer()) {
					handle = it->get<std::int32_t>();
				}
			}
			if (handle == 0) {
				handle = g_lastHandle;
			}
			bool ok = false;
			if (auto* api = SceneAPI(); api && handle != 0) {
				ok = api->StopScene(handle);
			}
			if (ok) {
				std::erase(g_closeStops, handle);
				if (handle == g_lastHandle) {
					g_lastHandle = 0;
				}
			}
			REX::DEBUG("[UI] osf.animation.stop handle={} -> {}", handle, ok);
		}

		// Advance a running scene one stage (its default advance edge; past the last stage this
		// ends the scene, same as the engine Space binding). The view is the ONLY advance channel
		// for browser launches: while the OSF UI overlay captures input its WndProc swallow starves
		// the engine of keyboard, so InputService never sees Space — and an NPC-only cast never
		// engages a director grant at all. Handle 0 targets the last browser launch, like stop.
		void OnAdvance(const char*, const char* a_payload, const char*, void*) noexcept
		{
			const json   j = ParsePayload(a_payload);
			std::int32_t handle = 0;
			if (j.is_object()) {
				if (const auto it = j.find("handle"); it != j.end() && it->is_number_integer()) {
					handle = it->get<std::int32_t>();
				}
			}
			if (handle == 0) {
				handle = g_lastHandle;
			}
			bool ok = false;
			if (auto* api = SceneAPI(); api && handle != 0) {
				ok = api->Advance(handle);
			}
			REX::DEBUG("[UI] osf.animation.advance handle={} -> {}", handle, ok);
		}

		void OnWheelGet(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const json j = ParsePayload(a_payload);
			const std::string prefix = j.is_object() && j.contains("tagPrefix") && j["tagPrefix"].is_string()
			                             ? j["tagPrefix"].get<std::string>()
			                             : std::string{ "player.emote." };
			SendJson(a_srcView, "osf.animation.wheel.data", BuildWheelData(prefix));
		}

		// Persist a complete, ordered animation-wheel loadout. The view materializes
		// the installed defaults on the first edit. `reset:true` returns to derivation.
		void OnWheelSet(const char*, const char* a_payload, const char*, void*) noexcept
		{
			const json j = ParsePayload(a_payload);
			if (!j.is_object()) {
				return;
			}
			if (j.value("reset", false)) {
				if (Serialization::WheelPins::Reset()) {
					REX::DEBUG("[UI] animation wheel reset to installed defaults");
					PushCatalogUpdate();
				}
				return;
			}
			const auto it = j.find("entries");
			if (it == j.end() || !it->is_array() || it->size() > 12) {
				REX::WARN("[UI] osf.animation.wheel.set refused malformed/oversized loadout");
				return;
			}
			std::vector<Serialization::WheelPins::Entry> entries;
			entries.reserve(it->size());
			for (const auto& value : *it) {
				if (!value.is_object()) {
					REX::WARN("[UI] osf.animation.wheel.set refused a non-object entry");
					return;
				}
				const auto sit = value.find("scene");
				if (sit == value.end() || !sit->is_string()) {
					REX::WARN("[UI] osf.animation.wheel.set refused an entry without a scene id");
					return;
				}
				Serialization::WheelPins::Entry entry;
				entry.scene = sit->get<std::string>();
				if (const auto stit = value.find("stage"); stit != value.end()) {
					if (!stit->is_number_integer()) {
						REX::WARN("[UI] osf.animation.wheel.set refused a non-integer stage");
						return;
					}
					entry.stage = stit->get<std::int32_t>();
				}

				const auto def = Registry::SceneRegistry::GetSingleton().Find(entry.scene);
				const bool eligible = def && (entry.stage < 0 ? IsWheelScene(*def) : WheelStage(*def, entry.stage) != nullptr);
				if (!eligible) {
					REX::WARN("[UI] osf.animation.wheel.set refused ineligible animation '{}' stage {}", entry.scene, entry.stage);
					return;
				}
				entries.push_back(std::move(entry));
			}
			if (Serialization::WheelPins::SetEntries(entries)) {
				REX::DEBUG("[UI] animation wheel customized with {} entr{}", entries.size(), entries.size() == 1 ? "y" : "ies");
				PushCatalogUpdate();
			}
		}

		// ---- nearby-actor enumeration ----------------------------------------
		// ProcessLists::highActorHandles (CommonLibSF, +0x60) is the primitive Scan Nearby wants: near-player, fully-3D actors that SPAN the loaded cell grid (interior + exterior neighbours)
		// We ONLY touch the high list — lowActorHandles holds 600-1200 partially- loaded actors whose vfuncs __fastfail uncatchably.

		std::uintptr_t VtableAddr(REL::ID a_id) { return REL::Relocation<std::uintptr_t>{ a_id }.address(); }
		void EnumerateHighActors(std::vector<RE::Actor*>& a_out)
		{
			auto* pl = RE::ProcessLists::GetSingleton();
			if (!pl) {
				return;
			}

			auto&               handles = pl->highActorHandles;
			const std::uint32_t size = handles.size();
			if (size == 0 || size > 0x4000) {
				return;
			}

			// The list can hold mixed TESObjectREFR*/Actor*, so confirm each resolved object is a real Actor by its primary vtable before use.
			const std::uintptr_t actorVtbl = VtableAddr(REL::ID(451614));
			a_out.reserve(a_out.size() + size);
			for (std::uint32_t i = 0; i < size; ++i) {
				RE::BSPointerHandle<RE::Actor>& h = handles[i];
				if (!static_cast<bool>(h)) {
					continue;
				}
				const RE::NiPointer<RE::Actor> p = h.get();  // GetSmartPointer ID 35638; self-guards bad handles
				RE::Actor* const               a = p.get();
				if (!a || *reinterpret_cast<std::uintptr_t*>(a) != actorVtbl) {
					continue;
				}
				a_out.push_back(a);
			}
		}

		// Targets deeper than this (view depth from the pick camera) get no marker and
		// are therefore unclickable. There is no mapped physics ray to test real
		// occlusion, so range is the working stand-in: it ends "selected an actor way
		// off across the map, through a building" (in-game report), and SCAN remains
		// the tool for gathering anything beyond sight.
		constexpr float kMaxPickDepth = 50.0f;

		// Scale for the acceptance-ellipse MINIMUM radii by view depth. The generous
		// floors (38x52 px for actors) keep close targets easy to hit, but a distant
		// actor renders ~20 px tall — a full-size floor let clicks land on far-away
		// targets "behind" nearer world geometry the cursor was visually on. Full
		// size inside ~12, shrinking to a third of it from ~40 out: distant targets
		// demand deliberate, precise aim.
		float PickFloorScale(float a_depth)
		{
			return a_depth > 0.0f ? std::clamp(12.0f / a_depth, 0.3f, 1.0f) : 1.0f;
		}

		// The screen-space acceptance ellipse of a pickable target: projected bound
		// center plus clamped pixel radii. The view renders these as hover markers
		// AND resolves the click against them (hottestPickTarget), so what the user
		// sees lit is by construction what a click selects.
		struct PickScreenBound
		{
			RE::NiPoint3 screen;  // normalized center; z = view depth
			float        radiusX{ 0.0f };
			float        radiusY{ 0.0f };
		};

		// Actor hit regions come from the rendered skeleton, not worldBound: the
		// cull sphere is expanded and re-centered by weapons, animation, and OSF's
		// own compose-root cull pin, which made some actors unclickable — their
		// acceptance ellipse sat nowhere near the visible body. Both landmarks
		// must be RENDERED BONES (C_Head, C_Hips): bones are exactly where the
		// visible body is, while the data3D root's translate proved to drift away
		// from the body (runtime-observed: actors OSF had animated missed every
		// click with scores of 8-260 while furniture picks kept landing). Feet
		// are estimated by mirroring the head across the hips along the body
		// axis, spanning standing AND prone poses; the ellipse is axis-aligned,
		// so each radius covers whichever span component runs its way.
		bool ActorScreenCapsule(const SafeViewProjection& a_projection, RE::Actor* a_actor,
			float a_width, float a_height, PickScreenBound& a_out)
		{
			RE::NiPointer<RE::NiAVObject> root;
			{
				const auto loaded = a_actor->loadedData.LockRead();
				if (*loaded) {
					root = (*loaded)->data3D;
				}
			}
			if (!root) {
				return false;
			}
			static const RE::BSFixedString headName{ "C_Head" };
			static const RE::BSFixedString hipsName{ "C_Hips" };
			RE::NiAVObject* head = root->GetObjectByName(headName);
			RE::NiAVObject* hips = root->GetObjectByName(hipsName);
			RE::NiPoint3    headWorld;
			RE::NiPoint3    baseWorld;
			if (head && hips) {
				headWorld = head->world.translate + RE::NiPoint3{ 0.0f, 0.0f, 0.12f };
				baseWorld = hips->world.translate + (hips->world.translate - head->world.translate);
			} else if (RenderedActorLabelPoint(a_actor, headWorld)) {
				// Creature rigs without the humanoid bones: label point (clamped
				// worldBound top) against the root translate, as before.
				baseWorld = root->world.translate;
			} else {
				return false;
			}
			RE::NiPoint3 headScreen;
			RE::NiPoint3 baseScreen;
			if (!a_projection.Project(headWorld, headScreen) || !a_projection.Project(baseWorld, baseScreen)) {
				return false;
			}
			const float dxPx = std::abs(headScreen.x - baseScreen.x) * a_width;
			const float dyPx = std::abs(headScreen.y - baseScreen.y) * a_height;
			const float span = std::hypot(dxPx, dyPx);
			a_out.screen = {
				(headScreen.x + baseScreen.x) * 0.5f,
				(headScreen.y + baseScreen.y) * 0.5f,
				std::min(headScreen.z, baseScreen.z),
			};
			const float minScale = PickFloorScale(a_out.screen.z);
			a_out.radiusX = std::clamp(std::max(dxPx * 0.62f, span * 0.22f), 38.0f * minScale, 260.0f);
			a_out.radiusY = std::clamp(std::max(dyPx * 0.62f, span * 0.22f), 52.0f * minScale, 320.0f);
			return true;
		}

		bool ComputePickScreenBound(const SafeViewProjection& a_projection, RE::TESObjectREFR* a_ref, bool a_actor,
			float a_width, float a_height, PickScreenBound& a_out)
		{
			if (a_actor && ActorScreenCapsule(a_projection, static_cast<RE::Actor*>(a_ref), a_width, a_height, a_out)) {
				return true;
			}
			RE::NiPoint3 center;
			float radius = 0.0f;
			if (!RenderedBound(a_ref, a_actor, center, radius) || !a_projection.Project(center, a_out.screen)) {
				return false;
			}
			const float projectedRadius = a_projection.ProjectedRadius(center, radius, a_width, a_height);
			const float minScale = PickFloorScale(a_out.screen.z);
			a_out.radiusX = std::clamp(projectedRadius * 1.15f, (a_actor ? 38.0f : 30.0f) * minScale, 220.0f);
			a_out.radiusY = std::clamp(projectedRadius * 1.15f, (a_actor ? 52.0f : 30.0f) * minScale, 260.0f);
			return true;
		}

		void SendScreenPick(const char* a_view, const std::string& a_slot, RE::TESObjectREFR* a_ref)
		{
			json reply{
				{ "slot", a_slot },
				{ "valid", a_ref != nullptr },
				{ "token", 0 },
				{ "name", "" },
				{ "formId", 0 },
			};
			if (a_ref) {
				const std::int32_t token = AllocToken(a_ref);
				reply["token"] = token;
				reply["name"] = ScanLabel(a_ref);
				reply["formId"] = a_ref->GetFormID();
				reply["species"] = a_ref->IsActor() ? Util::ActorSpecies(static_cast<RE::Actor*>(a_ref)) : std::string{};
				reply["sex"] = RefSexTag(a_ref);
				if (auto* player = RE::PlayerCharacter::GetSingleton()) {
					reply["distance"] = std::sqrt(player->GetPosition().GetSquaredDistance(a_ref->GetPosition())) / 70.0f;
				}
			}
			SendJson(a_view, "osf.animation.pick", reply);
		}

		// The view resolves a click against the SAME marker geometry it renders (the hot
		// marker from projectPickables) and sends that target's token, so the target the
		// user saw lit is by construction the one picked — no second projection pass at
		// click time that could disagree with the markers (stale camera, frame skew).
		// This side only re-validates that the token still names a live, eligible target.
		void OnPickScreen(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const json         j = ParsePayload(a_payload);
			const std::string  slot = j.is_object() && j.value("slot", std::string{}) == "furniture" ? "furniture" : "actor";
			const std::int32_t token = j.is_object() ? j.value("token", 0) : 0;
			auto*              player = RE::PlayerCharacter::GetSingleton();
			RE::TESObjectREFR* ref = token != 0 ? ResolveToken(token) : nullptr;
			if (ref && player) {
				bool eligible;
				if (slot == "actor") {
					auto* actor = ref->IsActor() ? static_cast<RE::Actor*>(ref) : nullptr;
					eligible = actor && !actor->IsPlayerRef() && !actor->IsDead();
				} else {
					const auto base = ref->GetBaseObject();
					eligible = !ref->IsDeleted() && base && base->Is(RE::FormType::kFURN);
				}
				if (!eligible || player->GetPosition().GetSquaredDistance(ref->GetPosition()) > 4096.0f * 4096.0f) {
					ref = nullptr;
				}
			} else {
				ref = nullptr;
			}
			REX::DEBUG("[UI] world PICK {} token={} -> {}", slot, token,
				ref ? std::format("'{}' ({:08X})", ScanLabel(ref), ref->GetFormID()) : "no longer a valid target");
			SendScreenPick(a_srcView, slot, ref);
		}

		// While a pick is armed the view polls this (~10 Hz) and marks pickable
		// targets — hover-only for actors, every candidate for furniture. Each item
		// carries the target's token, the marker anchor, and the acceptance ellipse
		// (ComputePickScreenBound). This is the ONLY projection pass in the pick
		// flow: the view renders these markers, resolves the click against them,
		// and sends back the hot marker's token (OnPickScreen just validates it).
		void OnProjectPickables(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const json        j = ParsePayload(a_payload);
			const std::string slot = j.is_object() && j.value("slot", std::string{}) == "furniture" ? "furniture" : "actor";
			const float       width = std::clamp(j.is_object() ? j.value("width", 0.0f) : 0.0f, 320.0f, 10000.0f);
			const float       height = std::clamp(j.is_object() ? j.value("height", 0.0f) : 0.0f, 200.0f, 10000.0f);
			const auto        projection = CurrentViewProjection(width, height);
			auto*             player = RE::PlayerCharacter::GetSingleton();

			const bool actorSlot = slot == "actor";

			// Gather every candidate WITH its screen bound, then gate and rank on that
			// bound: on-screen and within kMaxPickDepth of the pick camera. Eligibility
			// is decided HERE (a marker IS pickability); OnPickScreen re-checks only to
			// catch a target that died or left range between the marker poll and the click.
			struct Candidate
			{
				RE::TESObjectREFR* ref;
				PickScreenBound    bound;
			};
			std::vector<Candidate> candidates;
			const auto consider = [&](RE::TESObjectREFR* a_ref) {
				PickScreenBound bound;
				if (!ComputePickScreenBound(*projection, a_ref, actorSlot, width, height, bound)) {
					return;
				}
				if (bound.screen.z > kMaxPickDepth) {
					return;  // beyond picking range — SCAN is the tool for those
				}
				if (bound.screen.x < -0.1f || bound.screen.x > 1.1f ||
					bound.screen.y < -0.1f || bound.screen.y > 1.1f) {
					return;  // comfortably off-screen — never hoverable
				}
				candidates.push_back({ a_ref, bound });
			};

			if (projection && player && actorSlot) {
				std::vector<RE::Actor*> actors;
				EnumerateHighActors(actors);
				for (RE::Actor* actor : actors) {
					if (!actor || actor->IsPlayerRef() || actor->IsDead()) {
						continue;
					}
					consider(actor);
				}
			} else if (projection && player) {
				if (auto* tes = RE::TES::GetSingleton()) {
					RE::NiPoint3A origin{};
					origin.x = player->GetPosition().x;
					origin.y = player->GetPosition().y;
					origin.z = player->GetPosition().z;
					tes->ForEachReferenceInRange(origin, 4096.0f, [&](const RE::NiPointer<RE::TESObjectREFR>& a_ref) {
						RE::TESObjectREFR* ref = a_ref.get();
						const auto base = ref ? ref->GetBaseObject() : nullptr;
						if (ref && !ref->IsPlayerRef() && !ref->IsDeleted() && base && base->Is(RE::FormType::kFURN)) {
							consider(ref);
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
				}
			}

			// The caps keep crowded scenes bounded. Rank by VIEW DEPTH — the ordering
			// the user sees — so the cap always drops the deepest markers. (The old
			// player-distance sort starved visible actors of markers whenever the
			// orbit camera roamed away from the player.)
			std::sort(candidates.begin(), candidates.end(), [](const Candidate& a_lhs, const Candidate& a_rhs) {
				return a_lhs.bound.screen.z < a_rhs.bound.screen.z;
			});
			const std::size_t cap = actorSlot ? 48 : 32;
			json              items = json::array();
			for (const Candidate& candidate : candidates) {
				if (items.size() >= cap) {
					break;
				}
				// Marker anchor: actors use the rendered head the name labels sit on;
				// furniture floats the marker just above its rendered bound. The bound
				// center is the fallback either way.
				RE::NiPoint3 anchorScreen = candidate.bound.screen;
				RE::NiPoint3 anchorWorld;
				RE::NiPoint3 center;
				float        radius = 0.0f;
				const bool   anchored = actorSlot
				      ? RenderedActorLabelPoint(candidate.ref, anchorWorld)
				      : (RenderedBound(candidate.ref, false, center, radius) &&
							(anchorWorld = center + RE::NiPoint3{ 0.0f, 0.0f, std::clamp(radius, 0.25f, 1.4f) }, true));
				if (anchored) {
					RE::NiPoint3 projected;
					if (projection->Project(anchorWorld, projected)) {
						anchorScreen = projected;
					}
				}
				items.push_back(json{
					{ "token", AllocToken(candidate.ref) },
					{ "x", anchorScreen.x },
					{ "y", anchorScreen.y },
					{ "cx", candidate.bound.screen.x },
					{ "cy", candidate.bound.screen.y },
					{ "rx", candidate.bound.radiusX },
					{ "ry", candidate.bound.radiusY },
					{ "depth", candidate.bound.screen.z },
				});
			}
			SendJson(a_srcView, "osf.animation.pickTargets", json{ { "slot", slot }, { "items", std::move(items) } });
		}		// Nearby-furniture enumeration goes through RE::TES::ForEachReferenceInRange (CommonLibSF),
		// which spans the loaded interior cell or exterior grid — see OnScanNearby's furniture branch.

		// Distance math uses TESObjectREFR::GetPosition() (cached data.location), the same source the rest of OSF Animation uses for actor/anchor placement.
		void OnScanNearby(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const auto  scanStart = std::chrono::steady_clock::now();
			const json  j = ParsePayload(a_payload);
			std::string kind = "actor";
			std::string sceneId;
			float       radius = 4096.0f;  // ~58m; a room / nearby area
			if (j.is_object()) {
				if (const auto it = j.find("kind"); it != j.end() && it->is_string()) {
					kind = it->get<std::string>();
				}
				if (const auto it = j.find("sceneId"); it != j.end() && it->is_string()) {
					sceneId = it->get<std::string>();
				}
				if (const auto it = j.find("radius"); it != j.end() && it->is_number()) {
					radius = it->get<float>();
				}
			}
			const bool wantActor = (kind != "furniture");

			json reply;
			reply["kind"] = kind;
			reply["items"] = json::array();

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				REX::DEBUG("[UI] osf.animation.scanNearby kind={} -> no player", kind);
				SendJson(a_srcView, "osf.animation.scanResults", reply);
				return;
			}

			const RE::NiPoint3 origin = player->GetPosition();
			const float        radiusSq = (radius > 0.0f) ? radius * radius : 4096.0f * 4096.0f;

			struct Hit
			{
				RE::TESObjectREFR* ref;
				float              distSq;
				std::int32_t       sceneCount = -1;      // furniture only: total anchor-bound scenes that accept it (-1 = n/a)
				std::int32_t       customCount = -1;     // furniture only: of those, how many are custom (non-library) scenes
				RE::BGSKeyword*    matchedKw = nullptr;  // furniture only: the anchor keyword that matched (labels unnamed markers)
			};
			std::vector<Hit> hits;
			// Collect candidate pointers + distance only; serialize (GetDisplayFullName / token minting) afterwards so the heavy work stays out of any engine lock.
			if (wantActor) {
				std::vector<RE::Actor*> actors;
				EnumerateHighActors(actors);
				for (RE::Actor* actor : actors) {
					if (!actor || actor->IsPlayerRef() || actor->IsDeleted() || actor->IsDead()) {
						continue;
					}
					const float distSq = origin.GetSquaredDistance(actor->GetPosition());
					if (distSq <= radiusSq) {
						hits.push_back({ actor, distSq });
					}
				}
			} else if (auto* tes = RE::TES::GetSingleton()) {
				// Inverted anchor index, built fresh each scan: keyword -> accepting def indices and base form -> accepting def indices. 
				// Matching a ref then costs one HasKeyword per UNIQUE keyword instead of per (def x keyword) 
				std::vector<bool>                                               defCustom;  // def index -> custom (non-library) scene
				std::unordered_map<RE::BGSKeyword*, std::vector<std::uint32_t>> kwDefs;
				std::unordered_map<RE::TESFormID, std::vector<std::uint32_t>>   baseDefs;
				const auto                                                      addDef = [&](const Registry::SceneDef& d) {
					if (!d.RequiresAnchor() || !d.clipsAvailable) {
						return;
					}
					const auto idx = static_cast<std::uint32_t>(defCustom.size());
					defCustom.push_back(!d.library);
					// Resolve each keyword id fresh for this scan; the pointer only lives as a map key
					// for the duration of the sweep (HasKeyword needs the form, not the id).
					for (const auto kwId : d.anchorKeywords) {
						if (auto* kw = RE::TESForm::LookupByID<RE::BGSKeyword>(kwId)) {
							kwDefs[kw].push_back(idx);
						}
					}
					for (const auto b : d.anchorBaseForms) {
						baseDefs[b].push_back(idx);
					}
				};
				auto& reg = Registry::SceneRegistry::GetSingleton();
				if (!sceneId.empty()) {
					if (const auto def = reg.Find(sceneId)) {
						addDef(*def);
					}
				}
				if (defCustom.empty()) {
					reg.ForEachDef(addDef);
				}

				RE::NiPoint3A originA{};
				originA.x = origin.x;
				originA.y = origin.y;
				originA.z = origin.z;
				// Anchor keywords live on BASE records (the ESM extractor reads them from FURN/ACTI
				// forms), so keyword probing is memoized PER UNIQUE BASE: a POI places the same chair
				// or marker base dozens of times, and the unique-keyword set is ~150 strong with the
				// vanilla packs — probing it per REF (refs x keywords engine calls) was still a hitch.
				struct BaseMatch
				{
					std::int32_t    accepts = 0;
					std::int32_t    customAccepts = 0;
					RE::BGSKeyword* kw = nullptr;  // a matching keyword (labels unnamed markers)
				};
				std::unordered_map<RE::TESFormID, BaseMatch> baseCache;
				std::vector<std::uint32_t>                   matched;  // scratch: accepting def indices
				// ForEachReferenceInRange spans the loaded interior cell or exterior grid and only
				// visits refs already within radius; we just filter to furniture our scenes anchor to.
				tes->ForEachReferenceInRange(originA, radius, [&](const RE::NiPointer<RE::TESObjectREFR>& a_ref) {
					RE::TESObjectREFR* ref = a_ref.get();
					if (ref && !ref->IsPlayerRef() && !ref->IsDeleted()) {
						const auto base = ref->GetBaseObject();
						if (!base) {
							return RE::BSContainer::ForEachResult::kContinue;  // anchors match by base form / base-record keywords
						}
						auto cit = baseCache.find(base->GetFormID());
						if (cit == baseCache.end()) {
							// First ref of this base: count every accepting def (not just the first) —
							// the view shows "unlocks N scenes" next to each nearby anchor.
							matched.clear();
							BaseMatch m;
							if (const auto it = baseDefs.find(base->GetFormID()); it != baseDefs.end()) {
								matched.insert(matched.end(), it->second.begin(), it->second.end());
							}
							for (const auto& [kw, idxs] : kwDefs) {
								if (ref->HasKeyword(kw)) {
									if (!m.kw) {
										m.kw = kw;  // any matching keyword will do
									}
									matched.insert(matched.end(), idxs.begin(), idxs.end());
								}
							}
							// A def can match via its base form AND several keywords — count it once.
							std::sort(matched.begin(), matched.end());
							matched.erase(std::unique(matched.begin(), matched.end()), matched.end());
							m.accepts = static_cast<std::int32_t>(matched.size());
							for (const auto i : matched) {
								if (defCustom[i]) {
									m.customAccepts++;  // custom (authored) scene, vs a generated vanilla-library pack
								}
							}
							cit = baseCache.emplace(base->GetFormID(), m).first;
						}
						const BaseMatch& m = cit->second;
						if (m.accepts != 0) {
							hits.push_back({ ref, origin.GetSquaredDistance(ref->GetPosition()), m.accepts, m.customAccepts, m.kw });
						}
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}

			std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.distSq < b.distSq; });

			// Cap named refs and unnamed AI markers SEPARATELY: markers are dense (every sandbox
			// cell has dozens) and a single shared cap would crowd real furniture off the list.
			constexpr std::size_t kMaxPerGroup = 40;
			std::size_t           namedCount = 0, markerCount = 0;
			for (const auto& h : hits) {
				const char* nm = h.ref->GetDisplayFullName();
				// Furniture with no display name = invisible AI/idle marker (or unnamed outpost
				// piece) — still a legitimate anchor, but the view lists it under its own group.
				const bool marker = !wantActor && !(nm && nm[0]);
				auto&      count = marker ? markerCount : namedCount;
				if (count >= kMaxPerGroup) {
					continue;
				}
				count++;
				const std::int32_t token = AllocToken(h.ref);
				json               item = {
					{ "token", token },
					{ "name", ScanLabel(h.ref, h.matchedKw) },
					{ "formId", h.ref->GetFormID() },
					{ "distance", std::sqrt(h.distSq) / 70.0f },  // game units -> ~meters
					{ "isActor", h.ref->IsActor() },
					{ "marker", marker },
					{ "species", h.ref->IsActor() ? Util::ActorSpecies(static_cast<RE::Actor*>(h.ref)) : std::string{} },
					{ "sex", RefSexTag(h.ref) },
				};
				if (h.sceneCount >= 0) {
					item["sceneCount"] = h.sceneCount;
					item["customCount"] = h.customCount;  // subset that is custom (authored), not vanilla library
				}
				reply["items"].push_back(std::move(item));
			}
			const auto scanMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - scanStart).count();
			REX::DEBUG("[UI] osf.animation.scanNearby kind={} radius={} -> {} hit(s) in {} ms", kind, radius, hits.size(), scanMs);
			SendJson(a_srcView, "osf.animation.scanResults", reply);
		}

		// Which anchor-bound scenes accept a keyed furniture ref. The view filters its browse
		// list with this: free-space scenes always play; anchor-bound ones only via a match.
		void OnAnchorMatch(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const json   j = ParsePayload(a_payload);
			std::int32_t token = 0;
			if (j.is_object()) {
				if (const auto it = j.find("token"); it != j.end() && it->is_number_integer()) {
					token = it->get<std::int32_t>();
				}
			}

			json reply;
			reply["token"] = token;
			reply["sceneIds"] = json::array();

			RE::TESObjectREFR* ref = ResolveToken(token);
			if (ref) {
				Matchmaking::AnchorMatchCache cache(ref);  // one HasKeyword per unique keyword across the def sweep
				Registry::SceneRegistry::GetSingleton().ForEachDef([&reply, &cache](const Registry::SceneDef& d) {
					if (d.clipsAvailable && d.RequiresAnchor() && cache.Accepts(d)) {
						reply["sceneIds"].push_back(d.id);
					}
				});
			}
			REX::DEBUG("[UI] osf.animation.anchorMatch token={} -> {} scene(s)", token, reply["sceneIds"].size());
			SendJson(a_srcView, "osf.animation.anchorMatch", reply);
		}

		// The view reports every visibility change (ui.visibility -> osf.opened / osf.closed): the
		// input hook learns whether a UI cursor is on screen (visible = the scene-orbit camera
		// steers by LMB-drag; hidden = free-look).
		void OnOpened(const char*, const char*, const char* a_srcView, void*) noexcept
		{
			g_viewVisible = true;
			Input::InputService::GetSingleton().SetUiCursorVisible(true);
			// A wheel open is pending: replay the mode switch (idempotent view-side). With an
			// OSF UI that queues sends to a not-yet-visible view (bridge MINOR >= 2) the
			// immediate OpenWheel push already landed pre-paint and this is a no-op re-send;
			// on an older host this replay still delivers the wheel, just after first paint
			// (a brief console flash instead of a broken wheel).
			if (g_wheel.active) {
				SendWheelMode();
			}
			// Current live-scene list (NPC scenes may still be running from an earlier
			// session) — the reopened browser is their stop surface.
			PushActiveScenes();
			// What the reticle was on when the browser opened, so the view can seed the crew /
			// anchor with it instead of defaulting to the player. Same capture PICK resolves
			// (the engine has nulled the live slot by now); a furniture ref is only offered as
			// an anchor if some scene actually accepts it, so aiming at a crate seeds nothing.
			bool actorTargeted = false;
			if (g_openPickToken != 0) {
				if (RE::TESObjectREFR* ref = ResolveToken(g_openPickToken)) {
					const bool isActor = ref->IsActor();
					actorTargeted = isActor;
					bool       usable = isActor;
					if (!isActor) {
						Matchmaking::AnchorMatchCache cache(ref);
						Registry::SceneRegistry::GetSingleton().ForEachDef([&usable, &cache](const Registry::SceneDef& d) {
							if (!usable && d.clipsAvailable && d.RequiresAnchor() && cache.Accepts(d)) {
								usable = true;
							}
						});
					}
					if (usable) {
						json p;
						p["slot"] = isActor ? "actor" : "furniture";
						p["token"] = g_openPickToken;
						p["name"] = ScanLabel(ref);
						p["formId"] = ref->GetFormID();
						p["species"] = isActor ? Util::ActorSpecies(static_cast<RE::Actor*>(ref)) : std::string{};
						p["sex"] = RefSexTag(ref);
						REX::DEBUG("[UI] OnOpened: seeding view with crosshair target '{}' as {}", ScanLabel(ref), p["slot"].get<std::string>());
						SendJson(a_srcView, "osf.animation.openTarget", p);
					}
				}
			}
			// No actor under the reticle at open -> the view defaults the cast to the PLAYER,
			// and a first-person player can't see the body the browser is about to animate.
			// One-shot kick to third person (nothing restored on close — the player zooms back
			// in themself). The wheel is a quick pick overlay, not a browsing session — skip it.
			if (!g_wheel.active && !actorTargeted) {
				Camera::CameraService::GetSingleton().KickToThirdPerson();
			}
		}

		void OnClosed(const char*, const char*, const char*, void*) noexcept
		{
			g_viewVisible = false;
			Input::InputService::GetSingleton().SetUiCursorVisible(false);
			Camera::CameraService::GetSingleton().ReleaseBrowseOrbit();  // drag-to-look never outlives the browser
			g_wheel = {};  // any hide ends wheel mode; the next open starts clean
			g_openPickToken = 0;  // the open-time crosshair capture never outlives its session
			g_orbitSpaceNoticed = false;  // re-arm the in-space orbit notice for the next session
			// Abort console-launched PLAYER scenes (see g_closeStops): the browser was the only
			// stop button, so one outliving it would leave the player stuck. NPC-only scenes
			// are not in the list — they keep running until stopped from a reopened browser.
			if (!g_closeStops.empty()) {
				if (auto* api = SceneAPI()) {
					for (const std::int32_t h : g_closeStops) {
						if (api->StopScene(h)) {
							REX::INFO("[UI] browser closed — aborted live scene {:#010x}", h);
						}
					}
				}
				g_closeStops.clear();
				g_lastHandle = 0;
			}
		}

		// The view cannot hide itself (visibility is host-driven); this is its close button.
		// Generic — the wheel sends it on cancel and after a successful pick. Handlers already
		// run on the game main thread and RequestMenu is thread-safe/queued, so no task hop.
		void OnRequestClose(const char*, const char*, const char*, void*) noexcept
		{
			if (!g_ui.Has(OSFUI::API::Feature::kRequestMenu)) {
				REX::WARN("[UI] osf.animation.requestClose: installed OSF UI has no RequestMenu (bridge MINOR < 1) — ignored");
				return;
			}
			const bool ok = g_ui.RequestMenu(kViewId, false);
			REX::DEBUG("[UI] osf.animation.requestClose -> RequestMenu('{}', close) -> {}", kViewId, ok);
		}

		// World-area drag/wheel from the view (osf.orbit {dx,dy,wheel}) — the overlay consumes all
		// game input while open, so this is the ONLY mouse path to the orbit camera while browsing.
		void OnOrbit(const char*, const char* a_payloadJson, const char* a_srcView, void*) noexcept
		{
			const json p = ParsePayload(a_payloadJson);
			if (!p.is_object()) {
				return;
			}
			if (!g_viewVisible) {
				// A delta flush that raced the close (queued before osf.closed, delivered after).
				// Engaging here would orbit a camera nobody can ever release — drop it.
				REX::TRACE("[UI] osf.animation.orbit after close — dropped");
				return;
			}
			// First world drag of this browser session: engage the BROWSE ORBIT so there is always
			// something to steer — without it the deltas below are dropped unless a scene_orbit scene
			// happens to be running (and the vanilla camera is frozen by the overlay regardless).
			// Focal point: the player's live scene cast if they are mid-scene (e.g. an emote with
			// camera "none"), else the player. Handlers run on the game main thread.
			auto& cam = Camera::CameraService::GetSingleton();
			if (!cam.BrowseOrbitHeld()) {
				std::vector<std::uint32_t> cast;
				auto* api = SceneAPI();
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (api && player) {
					if (const std::int32_t live = api->GetSceneForActor(player); live != 0) {
						RE::Actor* buf[8] = {};
						const auto n = std::min<std::uint32_t>(api->GetSceneParticipants(live, buf, 8), 8);
						for (std::uint32_t i = 0; i < n; i++) {
							if (buf[i]) {
								cast.push_back(buf[i]->formID);
							}
						}
					}
				}
				if (!cam.EnsureBrowseOrbit(std::move(cast)) && !g_orbitSpaceNoticed) {
					// Aboard a ship in space the orbit can't engage (see EnsureBrowseOrbit) — the
					// drag silently does nothing, so tell the view why, once per session.
					g_orbitSpaceNoticed = true;
					json notice;
					notice["kind"] = "info";
					notice["text"] = "Camera orbit is unavailable in space — land to use it.";
					SendJson(a_srcView, "osf.animation.notice", notice);
				}
			}
			Input::InputService::GetSingleton().InjectOrbitDelta(
				p.value("dx", 0.0f), p.value("dy", 0.0f), p.value("wheel", 0.0f));
		}

		void OnBridgeReady(void*) noexcept
		{
			REX::DEBUG("[UI] OSF UI bridge ready — pushing catalog to view '{}'", kViewId);
			g_uiReady = true;
			SendJson(kViewId, "osf.animation.catalog.data", BuildCatalog(false));
		}
	}

	void PushCatalogUpdate()
	{
		if (!g_ui || !g_uiReady) {
			return;  // OSF UI absent or not ready yet — the ready push will carry current data
		}
		REX::DEBUG("[UI] clip durations updated — re-pushing catalog to view '{}'", kViewId);
		SendJson(kViewId, "osf.animation.catalog.data", BuildCatalog(false));
	}

	bool UIBridgeInstalled()
	{
		return g_ui.IsConnected();
	}

	void SetBrowserAutoMinimize(bool a_enabled)
	{
		g_browserAutoMinimize = a_enabled;
		REX::DEBUG("[UI] browser Auto-Minimize {}", a_enabled ? "enabled" : "disabled");
	}

	bool OpenBrowser()
	{
		if (!g_ui) {
			REX::WARN("[UI] OpenBrowser: OSF UI not present — nothing to open");
			return false;
		}
		// RequestMenu is an appended vmethod (bridge MINOR >= 1); the Client
		// wrapper would no-op it on an older host — gate explicitly so the
		// user gets an actionable message instead of a silent nothing.
		if (!g_ui.Has(OSFUI::API::Feature::kRequestMenu)) {
			REX::WARN("[UI] OpenBrowser: installed OSF UI is too old (bridge MINOR < 1) — update OSF UI to open the browser from native code");
			return false;
		}
		// Close any open game menus FIRST, so the browser opens over the world, not over a
		// still-open inventory or book. UIMessageQueue is main-thread-only and this can run on
		// a Papyrus VM thread, so the whole sequence rides an SFSE task; RequestMenu itself is
		// thread-safe and queued, keeping the hide -> open order.
		SFSE::GetTaskInterface()->AddTask([]() {
			// A normal browser open must never land in wheel mode: drop any pending wheel
			// state and, if the view is already up as the wheel, switch it back to the console.
			if (g_wheel.active) {
				g_wheel = {};
				SendJson(kViewId, "osf.animation.mode", json{ { "mode", "browser" } });
			}
			// Capture the reticle target NOW, before any menu (ours included) is up — once the
			// browser opens the engine nulls the slot, and PICK resolves this capture instead.
			g_openPickToken = 0;
			if (RE::TESObjectREFR* ref = CrosshairRef()) {
				g_openPickToken = AllocToken(ref);
				REX::DEBUG("[UI] OpenBrowser: captured crosshair target '{}' ({:08X}) for PICK", ScanLabel(ref), ref->GetFormID());
			}
			auto* ui = RE::UI::GetSingleton();
			auto* queue = RE::UIMessageQueue::GetSingleton();
			if (ui && queue) {
				// Cover the menus the browser might be opened over (inventory, data, a book).
				for (const char* menu : { "BookMenu", "InventoryMenu", "DataMenu" }) {
					if (ui->IsMenuOpen(menu)) {
						queue->AddMessage(menu, RE::UI_MESSAGE_TYPE::kHide);
						REX::DEBUG("[UI] OpenBrowser: closing '{}' before the browser", menu);
					}
				}
			}
			const bool ok = g_ui.RequestMenu(kViewId, true);
			REX::INFO("[UI] OpenBrowser: RequestMenu('{}', open) -> {}", kViewId, ok);
		});
		return true;
	}

	bool OpenWheel(std::string_view a_tagPrefix)
	{
		if (!g_ui) {
			REX::WARN("[UI] OpenWheel: OSF UI not present — nothing to open");
			UI::HudMessage::Error("OSF UI not present or too old");
			return false;
		}
		// Same gate as OpenBrowser: actionable message beats the wrapper's silent no-op.
		if (!g_ui.Has(OSFUI::API::Feature::kRequestMenu)) {
			REX::WARN("[UI] OpenWheel: installed OSF UI is too old (bridge MINOR < 1) — update OSF UI to use the animation wheel");
			UI::HudMessage::Error("OSF UI not present or too old");
			return false;
		}
		// Everything below touches refs and menus: ride an SFSE task so this stays callable
		// from any thread (the hotkey verb is already on the game thread — the task just runs
		// next frame).
		SFSE::GetTaskInterface()->AddTask([prefix = std::string{ a_tagPrefix }]() {
			g_wheel = {};
			g_wheel.active = true;
			g_wheel.tagPrefix = prefix.empty() ? std::string{ "player.emote." } : prefix;

			// Capture the crosshair target, gated harder than a browser pick: the wheel plays
			// on the target IMMEDIATELY on pick, so dead / fighting / non-human actors fall
			// back to a player-only wheel (a downgrade, not an error).
			if (RE::TESObjectREFR* ref = CrosshairRef(); ref && ref->IsActor()) {
				auto*       actor = static_cast<RE::Actor*>(ref);
				const char* reject = nullptr;
				if (actor->IsDead()) {
					reject = "dead";
				} else if (actor->combatController != nullptr) {
					// Combat via the member read, not the IsInCombat() virtual — that vtable
					// slot proved unreliable (see UISettings' combat guard).
					reject = "in combat";
				} else if (Util::ActorSpecies(actor) != "human") {
					reject = "non-human";  // no creature emote packs — human clips would T-pose them
				}
				if (reject) {
					REX::DEBUG("[UI] OpenWheel: crosshair target '{}' rejected ({}) — player-only wheel", ScanLabel(ref), reject);
				} else {
					g_wheel.targetToken = AllocToken(ref);
					g_wheel.targetName = ScanLabel(ref);
				}
			}

			// Same open sequence as OpenBrowser: hide the menus the hotkey could fire over.
			auto* ui = RE::UI::GetSingleton();
			auto* queue = RE::UIMessageQueue::GetSingleton();
			if (ui && queue) {
				for (const char* menu : { "BookMenu", "InventoryMenu", "DataMenu" }) {
					if (ui->IsMenuOpen(menu)) {
						queue->AddMessage(menu, RE::UI_MESSAGE_TYPE::kHide);
						REX::DEBUG("[UI] OpenWheel: closing '{}' before the wheel", menu);
					}
				}
			}
			// Immediate mode push covers a view that is already open (browser -> wheel switch);
			// the osf.opened replay covers a fresh view creation racing this send.
			SendWheelMode();
			const bool ok = g_ui.RequestMenu(kViewId, true);
			REX::INFO("[UI] OpenWheel: RequestMenu('{}', open) -> {} (prefix '{}', target: {})",
				kViewId, ok, g_wheel.tagPrefix, g_wheel.targetToken != 0 ? g_wheel.targetName : "player-only");
		});
		return true;
	}

	void InstallUIBridge()
	{
		using namespace OSFUI::API;

		if (!g_ui.Init()) {
			REX::INFO("[UI] OSF UI not present — in-game scene-browser view disabled (this is fine)");
			return;
		}

		g_ui.SetReadyCallback(&OnBridgeReady, nullptr);
		g_ui.RegisterCommand("osf.animation.catalog.get", &OnCatalogGet, nullptr);
		g_ui.RegisterCommand("osf.animation.library.get", &OnLibraryGet, nullptr);
		g_ui.RegisterCommand("osf.animation.pickCrosshair", &OnPickCrosshair, nullptr);
		g_ui.RegisterCommand("osf.animation.pickScreen", &OnPickScreen, nullptr);
		g_ui.RegisterCommand("osf.animation.projectPickables", &OnProjectPickables, nullptr);
		g_ui.RegisterCommand("osf.animation.projectActors", &OnProjectActors, nullptr);
		g_ui.RegisterCommand("osf.animation.scanNearby", &OnScanNearby, nullptr);
		g_ui.RegisterCommand("osf.animation.anchorMatch", &OnAnchorMatch, nullptr);
		g_ui.RegisterCommand("osf.animation.launch", &OnLaunch, nullptr);
		g_ui.RegisterCommand("osf.animation.stop", &OnStop, nullptr);
		g_ui.RegisterCommand("osf.animation.advance", &OnAdvance, nullptr);
		g_ui.RegisterCommand("osf.animation.wheel.get", &OnWheelGet, nullptr);
		g_ui.RegisterCommand("osf.animation.wheel.set", &OnWheelSet, nullptr);
		g_ui.RegisterCommand("osf.animation.opened", &OnOpened, nullptr);
		g_ui.RegisterCommand("osf.animation.closed", &OnClosed, nullptr);
		g_ui.RegisterCommand("osf.animation.orbit", &OnOrbit, nullptr);
		g_ui.RegisterCommand("osf.animation.requestClose", &OnRequestClose, nullptr);

		// Any scene lifecycle change (start, stage advance, any termination — including
		// natural timer/loop ends and Papyrus stops) refreshes the browser's ACTIVE list.
		// May fire off the game thread (a Papyrus StopScene), and the push touches
		// main-thread-only bridge state (the token map), so hop through the task queue —
		// which also lets the end path's ReleaseSlot retire the slot before ListScenes reads.
		Scene::SceneRuntime::GetSingleton().SetSceneObserver([]() {
			SFSE::GetTaskInterface()->AddTask([]() { PushActiveScenes(); });
		});

		// Bridge ABI 1.5: register our shipped views/osf.animation/browser/
		// folder as an openable surface — OSF UI's shipped config.views lists
		// only its built-ins, so without this call the view never loads.
		// Idempotent; older OSF UI runtimes lack the vmethod (wrapper-gated).
		if (g_ui.Has(Feature::kRegisterView)) {
			g_ui.RegisterView(kViewId);
		} else {
			REX::WARN("[UI] this OSF UI predates bridge ABI 1.5 (RegisterView) — the '{}' view only opens if the user's OSF UI config.json lists it in `views`", kViewId);
		}

		std::uint32_t mj = 0, mn = 0, pt = 0;
		g_ui.GetPluginVersion(mj, mn, pt);
		REX::INFO("[UI] OSF UI bridge connected (OSF UI v{}.{}.{}, protocol {}) — osf.animation.* commands registered",
			mj, mn, pt, g_ui.GetBridgeProtocolVersion());
		if (std::tie(mj, mn, pt) < std::tie(kOSFUITested[0], kOSFUITested[1], kOSFUITested[2])) {
			REX::WARN("[UI] installed OSF UI v{}.{}.{} predates the v{}.{}.{} this build was tested against — update it: {}",
				mj, mn, pt, kOSFUITested[0], kOSFUITested[1], kOSFUITested[2], kOSFUINexusURL);
		}
	}
}
