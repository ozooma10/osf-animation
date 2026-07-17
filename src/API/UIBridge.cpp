#include "API/UIBridge.h"

#include "API/OSFSceneAPI.h"  // OSFStartOptions + IOSFSceneAPI + kOSFSceneAPIVersion (in-process launch)
#include "API/OSFUI_API.h"    // the OSF UI bridge surface (JSON text only)
#include "Camera/CameraService.h"  // browse orbit: osf.orbit engages drag-to-look when no scene camera is live
#include "Input/InputService.h"  // osf.opened/closed -> UI-cursor mode for the orbit camera's drag-steer
#include "Matchmaking/Matchmaker.h"  // AnchorAccepts (osf.anchorMatch single-ref check)
#include "Registry/SceneRegistry.h"
#include "Serialization/ClipDurations.h"  // clip loop lengths for the catalog's time estimates
#include "UI/FirstRunHint.h"  // osf.opened -> count a browser open (retires the F10 hint)
#include "UI/HudMessage.h"    // OpenWheel's graceful-degrade popup (OSF UI absent/too old)
#include "Util/Species.h"     // catalog species tag + picked-actor species (creature filtering)
#include "Util/StringUtil.h"  // Util::ToLower

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// The in-process handle to OSF Animation's own exported scene API.
extern "C" OSF::API::IOSFSceneAPI* OSF_RequestSceneAPI(std::uint32_t a_abiVersion);

namespace OSF::API
{
	namespace
	{
		using json = nlohmann::json;

		// The bridge, fetched once at Install; nullptr => OSF UI absent (UI disabled).
		OSFUI::API::IOSFUIBridge* g_ui = nullptr;

		// Set by OnBridgeReady; unsolicited pushes (PushCatalogUpdate) are dropped before then.
		// Only touched on the game main thread (ready callback, command handlers, SFSE tasks).
		bool g_uiReady = false;

		// OSF Animation's own scene API, fetched lazily on first launch/stop.
		IOSFSceneAPI* g_scene = nullptr;

		// Last scene handle we launched, so an osf.stop with no handle can target it.
		std::int32_t g_lastHandle = 0;

		// Pending emote-wheel open (OpenWheel): the osf.mode push must survive the open race,
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
		constexpr const char* kViewId = "osf";

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
			g_ui->SendToWeb(a_view, a_type, text.c_str());
		}

		// Parse an inbound payload without throwing (handlers are noexcept). Returns a discarded value on malformed input; callers treat non-objects as empty.
		json ParsePayload(const char* a_json)
		{
			return json::parse(a_json ? a_json : "", nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
		}

		// The player's crosshair/command target as a validated object reference, or nullptr.
		// commandTarget is NOT guaranteed to be a ref: aiming at terrain or empty space yields
		// the CELL (or nothing). Main thread only.
		RE::TESObjectREFR* CrosshairRef()
		{
			auto*              player = RE::PlayerCharacter::GetSingleton();
			RE::TESObjectREFR* ref = player ? player->commandTarget : nullptr;
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
			SendJson(kViewId, "osf.mode", payload);
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

		// Actor count for a card: the declared role count, else the first playable stage's clip count (anonymous positional scenes have no roles[]). Reads only the def, so it is safe under the registry read lock (ForEachDef).
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
			const auto* def = reg.Find(a_sceneId);
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

		// Serialize the live scene registry to the osf.catalog.data array (a_library=false) or the osf.library.data array (a_library=true — the reference-library lane, e.g. the generated vanilla packs). 
		// Copies the fields out from under the registry read lock, then builds JSON afterwards
		json BuildCatalog(bool a_library)
		{
			struct StageCard
			{
				std::int32_t             index = 0;
				std::string              name;   // stage label ("" = unlabeled)
				std::vector<std::string> tags;
				std::int32_t             clipCount = 0;
				std::string              sig;    // clip-set signature (files joined) for de-dup
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
				std::string              species;  // skeleton family ("human" default) for the browser's per-actor filter
				std::vector<std::string> tags;
				std::uint32_t            actorCount = 0;
				std::vector<std::string> genders;
				bool                     requiresFurniture = false;
				std::vector<std::string> anchorNames;  // human labels for WHAT the scene anchors to ("Barstool", ...)
				bool                     unlisted = false;
				std::vector<StageCard>   stages;  // linear stages, in order (empty for a non-linear graph)
				float                    estSec = -1.0f;      // sum of known stage estimates (< 0 = none known)
				bool                     estPartial = false;  // at least one linear stage had no estimate
				bool                     openEnded = false;   // some stage holds until advanced
			};
			std::vector<Card> cards;
			Registry::SceneRegistry::GetSingleton().ForEachDef([&cards, a_library](const Registry::SceneDef& d) {
				if (d.library != a_library) {
					return;  // each lane serializes only its own scenes
				}
				Card c;
				c.id = d.id;
				c.title = d.name.empty() ? d.id : d.name;
				c.species = d.species.empty() ? std::string{ "human" } : d.species;
				c.tags = d.tags;
				c.actorCount = static_cast<std::uint32_t>(ActorCountOf(d));
				c.genders.reserve(d.roles.size());
				for (const auto& r : d.roles) {
					c.genders.emplace_back(GenderTag(r.gender));
				}
				c.requiresFurniture = d.RequiresAnchor();
				// Name the anchor, not just the fact of one: keyword edids prettify well
				// ("AnimFurnBarstool" -> "Barstool"); base-form anchors rarely retain an edid,
				// so those fall back to the form id — still identifiable, never blank.
				for (auto* kw : d.anchorKeywords) {
					if (std::string lbl = KeywordLabel(kw); !lbl.empty()) {
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
					{ "species", c.species },
					{ "tags", c.tags },
					{ "actorCount", c.actorCount },
					{ "genders", c.genders },
					{ "requiresFurniture", c.requiresFurniture },
					{ "anchors", c.anchorNames },
					{ "unlisted", c.unlisted },
					{ "stageCount", static_cast<std::int32_t>(c.stages.size()) },
					{ "stages", std::move(stages) },
					{ "estSec", secOrNull(c.estSec) },
					{ "estPartial", c.estPartial },
					{ "openEnded", c.openEnded },
				});
			}
			REX::DEBUG("[UI] {} built -> {} scene(s)", a_library ? "library" : "catalog", cards.size());
			return arr;
		}

		void OnCatalogGet(const char*, const char*, const char* a_srcView, void*) noexcept
		{
			SendJson(a_srcView, "osf.catalog.data", BuildCatalog(false));
		}

		// The library lane is static after load (generated packs, pack-authored durations), so the
		// view fetches it once on demand and caches — it is never re-pushed by catalog updates.
		void OnLibraryGet(const char*, const char*, const char* a_srcView, void*) noexcept
		{
			SendJson(a_srcView, "osf.library.data", BuildCatalog(true));
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

			RE::TESObjectREFR* ref = CrosshairRef();
			const bool         accept = ref && (!wantActor || ref->IsActor());

			json reply;
			reply["slot"] = slot;
			if (!accept) {
				reply["valid"] = false;
				reply["token"] = 0;
				reply["name"] = "";
				reply["formId"] = 0;
				REX::DEBUG("[UI] osf.pickCrosshair slot={} -> nothing valid under crosshair", slot);
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
				REX::DEBUG("[UI] osf.pickCrosshair slot={} -> token {} '{}' ({:08X})", slot, token, nm, ref->GetFormID());
			}
			SendJson(a_srcView, "osf.pick", reply);
		}

		void OnLaunch(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const json j = ParsePayload(a_payload);
			json       reply;

			const std::string sceneId = (j.is_object() && j.contains("sceneId") && j["sceneId"].is_string())
			                                ? j["sceneId"].get<std::string>()
			                                : std::string{};
			reply["sceneId"] = sceneId;

			auto fail = [&](const std::string& a_reason) {
				reply["ok"] = false;
				reply["handle"] = 0;
				reply["error"] = a_reason;
				REX::WARN("[UI] osf.launch '{}' refused: {}", sceneId, a_reason);
				SendJson(a_srcView, "osf.launchResult", reply);
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

			auto* api = SceneAPI();
			if (!api) {
				return fail("OSF Animation engine is not ready yet");
			}

			// Replace-in-place: if a cast member is already mid-scene, stop that scene first so this launch supersedes it 
			for (RE::Actor* a : actors) {
				const std::int32_t busy = api->GetSceneForActor(a);
				if (busy != 0) {
					api->StopScene(busy);
					if (busy == g_lastHandle) {
						g_lastHandle = 0;
					}
					REX::INFO("[UI] osf.launch '{}' superseding live scene {:#010x} (cast busy) — stopped it first", sceneId, busy);
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
			reply["ok"] = true;
			reply["handle"] = handle;
			REX::INFO("[UI] osf.launch '{}' -> handle {} ({} cast{})", sceneId, handle, actors.size(),
				furniture ? ", anchored" : "");
			SendJson(a_srcView, "osf.launchResult", reply);
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
			if (handle == g_lastHandle && ok) {
				g_lastHandle = 0;
			}
			REX::DEBUG("[UI] osf.stop handle={} -> {}", handle, ok);
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

		// Nearby-furniture enumeration goes through RE::TES::ForEachReferenceInRange (CommonLibSF),
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
				REX::DEBUG("[UI] osf.scanNearby kind={} -> no player", kind);
				SendJson(a_srcView, "osf.scanResults", reply);
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
					if (!d.RequiresAnchor()) {
						return;
					}
					const auto idx = static_cast<std::uint32_t>(defCustom.size());
					defCustom.push_back(!d.library);
					for (auto* kw : d.anchorKeywords) {
						if (kw) {
							kwDefs[kw].push_back(idx);
						}
					}
					for (const auto b : d.anchorBaseForms) {
						baseDefs[b].push_back(idx);
					}
				};
				auto& reg = Registry::SceneRegistry::GetSingleton();
				if (!sceneId.empty()) {
					if (const auto* def = reg.Find(sceneId)) {
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
				};
				if (h.sceneCount >= 0) {
					item["sceneCount"] = h.sceneCount;
					item["customCount"] = h.customCount;  // subset that is custom (authored), not vanilla library
				}
				reply["items"].push_back(std::move(item));
			}
			const auto scanMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - scanStart).count();
			REX::DEBUG("[UI] osf.scanNearby kind={} radius={} -> {} hit(s) in {} ms", kind, radius, hits.size(), scanMs);
			SendJson(a_srcView, "osf.scanResults", reply);
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
					if (d.RequiresAnchor() && cache.Accepts(d)) {
						reply["sceneIds"].push_back(d.id);
					}
				});
			}
			REX::DEBUG("[UI] osf.anchorMatch token={} -> {} scene(s)", token, reply["sceneIds"].size());
			SendJson(a_srcView, "osf.anchorMatch", reply);
		}

		// The view reports every visibility change (ui.visibility -> osf.opened / osf.closed): the
		// first-run F10 hint counts real opens, and the input hook learns whether a UI cursor is on
		// screen (visible = the scene-orbit camera steers by LMB-drag; hidden = free-look).
		void OnOpened(const char*, const char*, const char*, void*) noexcept
		{
			UI::FirstRunHint::OnMenuOpened();
			Input::InputService::GetSingleton().SetUiCursorVisible(true);
			// A wheel open is pending: replay the mode switch (idempotent view-side). With an
			// OSF UI that queues sends to a not-yet-visible view (bridge MINOR >= 2) the
			// immediate OpenWheel push already landed pre-paint and this is a no-op re-send;
			// on an older host this replay still delivers the wheel, just after first paint
			// (a brief console flash instead of a broken wheel).
			if (g_wheel.active) {
				SendWheelMode();
			}
		}

		void OnClosed(const char*, const char*, const char*, void*) noexcept
		{
			Input::InputService::GetSingleton().SetUiCursorVisible(false);
			Camera::CameraService::GetSingleton().ReleaseBrowseOrbit();  // drag-to-look never outlives the browser
			g_wheel = {};  // any hide ends wheel mode; the next open starts clean
		}

		// The view cannot hide itself (visibility is host-driven); this is its close button.
		// Generic — the wheel sends it on cancel and after a successful pick. Handlers already
		// run on the game main thread and RequestMenu is thread-safe/queued, so no task hop.
		void OnRequestClose(const char*, const char*, const char*, void*) noexcept
		{
			if ((g_ui->GetInterfaceVersion() & 0xFFFFu) < 1u) {
				REX::WARN("[UI] osf.requestClose: installed OSF UI has no RequestMenu (bridge MINOR < 1) — ignored");
				return;
			}
			const bool ok = g_ui->RequestMenu(kViewId, false);
			REX::DEBUG("[UI] osf.requestClose -> RequestMenu('{}', close) -> {}", kViewId, ok);
		}

		// World-area drag/wheel from the view (osf.orbit {dx,dy,wheel}) — the overlay consumes all
		// game input while open, so this is the ONLY mouse path to the orbit camera while browsing.
		void OnOrbit(const char*, const char* a_payloadJson, const char*, void*) noexcept
		{
			const json p = ParsePayload(a_payloadJson);
			if (!p.is_object()) {
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
				cam.EnsureBrowseOrbit(std::move(cast));
			}
			Input::InputService::GetSingleton().InjectOrbitDelta(
				p.value("dx", 0.0f), p.value("dy", 0.0f), p.value("wheel", 0.0f));
		}

		void OnBridgeReady(void*) noexcept
		{
			REX::DEBUG("[UI] OSF UI bridge ready — pushing catalog to view '{}'", kViewId);
			g_uiReady = true;
			SendJson(kViewId, "osf.catalog.data", BuildCatalog(false));
		}
	}

	void PushCatalogUpdate()
	{
		if (!g_ui || !g_uiReady) {
			return;  // OSF UI absent or not ready yet — the ready push will carry current data
		}
		REX::DEBUG("[UI] clip durations updated — re-pushing catalog to view '{}'", kViewId);
		SendJson(kViewId, "osf.catalog.data", BuildCatalog(false));
	}

	bool UIBridgeInstalled()
	{
		return g_ui != nullptr;
	}

	bool OpenBrowser()
	{
		if (!g_ui) {
			REX::WARN("[UI] OpenBrowser: OSF UI not present — nothing to open");
			return false;
		}
		// RequestMenu is an appended vmethod (bridge MINOR >= 1). An OSF UI built before it
		// has no such vtable slot; calling through it would be undefined. Gate on the live
		// interface version rather than the header constant we compiled against.
		if ((g_ui->GetInterfaceVersion() & 0xFFFFu) < 1u) {
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
				SendJson(kViewId, "osf.mode", json{ { "mode", "browser" } });
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
			const bool ok = g_ui->RequestMenu(kViewId, true);
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
		// Same live-interface gate as OpenBrowser: RequestMenu is an appended vmethod (MINOR >= 1).
		if ((g_ui->GetInterfaceVersion() & 0xFFFFu) < 1u) {
			REX::WARN("[UI] OpenWheel: installed OSF UI is too old (bridge MINOR < 1) — update OSF UI to use the emote wheel");
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
			const bool ok = g_ui->RequestMenu(kViewId, true);
			REX::INFO("[UI] OpenWheel: RequestMenu('{}', open) -> {} (prefix '{}', target: {})",
				kViewId, ok, g_wheel.tagPrefix, g_wheel.targetToken != 0 ? g_wheel.targetName : "player-only");
		});
		return true;
	}

	void InstallUIBridge()
	{
		using namespace OSFUI::API;

		g_ui = RequestBridge();
		if (!g_ui) {
			REX::INFO("[UI] OSF UI not present — in-game scene-browser view disabled (this is fine)");
			return;
		}

		g_ui->SetReadyCallback(&OnBridgeReady, nullptr);
		g_ui->RegisterCommand("osf.catalog.get", &OnCatalogGet, nullptr);
		g_ui->RegisterCommand("osf.library.get", &OnLibraryGet, nullptr);
		g_ui->RegisterCommand("osf.pickCrosshair", &OnPickCrosshair, nullptr);
		g_ui->RegisterCommand("osf.scanNearby", &OnScanNearby, nullptr);
		g_ui->RegisterCommand("osf.anchorMatch", &OnAnchorMatch, nullptr);
		g_ui->RegisterCommand("osf.launch", &OnLaunch, nullptr);
		g_ui->RegisterCommand("osf.stop", &OnStop, nullptr);
		g_ui->RegisterCommand("osf.opened", &OnOpened, nullptr);
		g_ui->RegisterCommand("osf.closed", &OnClosed, nullptr);
		g_ui->RegisterCommand("osf.orbit", &OnOrbit, nullptr);
		g_ui->RegisterCommand("osf.requestClose", &OnRequestClose, nullptr);

		// Bridge ABI 1.5: register our shipped views/osf/ folder as an openable
		// surface — OSF UI's shipped config.views lists only its built-ins, so
		// without this call the view never loads. Idempotent (a user config that
		// still lists "osf" is left untouched); older OSF UI runtimes lack the
		// vmethod, so gate on the interface MINOR.
		if ((g_ui->GetInterfaceVersion() & 0xFFFFu) >= 5u) {
			g_ui->RegisterView(kViewId);
		} else {
			REX::WARN("[UI] this OSF UI predates bridge ABI 1.5 (RegisterView) — the '{}' view only opens if the user's OSF UI config.json lists it in `views`", kViewId);
		}

		std::uint32_t mj = 0, mn = 0, pt = 0;
		g_ui->GetPluginVersion(mj, mn, pt);
		REX::INFO("[UI] OSF UI bridge connected (OSF UI v{}.{}.{}, protocol {}) — osf.* commands registered",
			mj, mn, pt, g_ui->GetBridgeProtocolVersion());
	}
}
