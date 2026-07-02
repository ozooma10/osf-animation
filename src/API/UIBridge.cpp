#include "API/UIBridge.h"

#include "API/OSFSceneAPI.h"  // OSFStartOptions + IOSFSceneAPI + kOSFSceneAPIVersion (in-process launch)
#include "API/OSFUI_API.h"    // the OSF UI bridge surface (JSON text only)
#include "Matchmaking/Matchmaker.h"  // AnchorAccepts (usable-furniture filter for Scan Nearby)
#include "Registry/SceneRegistry.h"
#include "Serialization/ClipDurations.h"  // clip loop lengths for the catalog's time estimates
#include "UI/FirstRunHint.h"  // osf.opened -> count a browser open (retires the F10 hint)
#include "Util/StringUtil.h"  // Util::ToLower

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
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
				std::vector<std::string> tags;
				std::uint32_t            actorCount = 0;
				std::vector<std::string> genders;
				bool                     requiresFurniture = false;
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
				c.tags = d.tags;
				c.actorCount = static_cast<std::uint32_t>(ActorCountOf(d));
				c.genders.reserve(d.roles.size());
				for (const auto& r : d.roles) {
					c.genders.emplace_back(GenderTag(r.gender));
				}
				c.requiresFurniture = d.RequiresAnchor();
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
					{ "tags", c.tags },
					{ "actorCount", c.actorCount },
					{ "genders", c.genders },
					{ "requiresFurniture", c.requiresFurniture },
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

			auto*              player = RE::PlayerCharacter::GetSingleton();
			RE::TESObjectREFR* ref = player ? player->commandTarget : nullptr;

			// commandTarget is NOT guaranteed to be an object reference; aiming at terrain or empty space yields the CELL (or nothing). 
			const bool isRef = ref && (ref->Is(RE::FormType::kREFR) || ref->Is(RE::FormType::kACHR));
			const bool accept = isRef && (!wantActor || ref->IsActor());

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
				const char*        nm = ref->GetDisplayFullName();
				reply["valid"] = true;
				reply["token"] = token;
				reply["name"] = nm ? nm : "";
				reply["formId"] = ref->GetFormID();
				REX::DEBUG("[UI] osf.pickCrosshair slot={} -> token {} '{}' ({:08X})", slot, token, nm ? nm : "", ref->GetFormID());
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
				std::int32_t       sceneCount = -1;  // furniture only: how many anchor-bound scenes accept it (-1 = n/a)
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
				std::vector<const Registry::SceneDef*> anchorDefs;
				auto&                                  reg = Registry::SceneRegistry::GetSingleton();
				if (!sceneId.empty()) {
					if (const auto* def = reg.Find(sceneId); def && def->RequiresAnchor()) {
						anchorDefs.push_back(def);
					}
				}
				if (anchorDefs.empty()) {
					reg.ForEachDef([&anchorDefs](const Registry::SceneDef& d) {
						if (d.RequiresAnchor()) {
							anchorDefs.push_back(&d);
						}
					});
				}

				RE::NiPoint3A originA{};
				originA.x = origin.x;
				originA.y = origin.y;
				originA.z = origin.z;
				// ForEachReferenceInRange spans the loaded interior cell or exterior grid and only
				// visits refs already within radius; we just filter to furniture our scenes anchor to.
				tes->ForEachReferenceInRange(originA, radius, [&](const RE::NiPointer<RE::TESObjectREFR>& a_ref) {
					RE::TESObjectREFR* ref = a_ref.get();
					if (ref && !ref->IsPlayerRef() && !ref->IsDeleted()) {
						// Count every accepting def (not just the first): the view shows "unlocks N
						// scenes" next to each nearby anchor so the pick is an informed one.
						std::int32_t accepts = 0;
						for (const auto* d : anchorDefs) {
							if (Matchmaking::AnchorAccepts(*d, ref)) {
								accepts++;
							}
						}
						if (accepts != 0) {
							hits.push_back({ ref, origin.GetSquaredDistance(ref->GetPosition()), accepts });
						}
						// DIAGNOSTIC (couch/bench hunt): log every FURN-type ref in radius, plus
						// EVERYTHING within ~5m, so nearby non-FURN "furniture" shows up too. Base
						// form IDs decode offline via osf-gergel-ebanex/furniture_dump.json.
						const auto          base = ref->GetBaseObject();
						const std::uint32_t type = base ? static_cast<std::uint32_t>(base->GetFormType()) : 0u;
						const float         dSq = origin.GetSquaredDistance(ref->GetPosition());
						if (type == 47 || dSq < (5.0f * 70.0f) * (5.0f * 70.0f)) {
							REX::DEBUG("[UI] scan cand: dist={:.1f}m base={:#010x} type={} accepts={} label='{}'",
								std::sqrt(dSq) / 70.0f, base ? base->GetFormID() : 0u, type, accepts, ScanLabel(ref));
						}
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}

			std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.distSq < b.distSq; });
			constexpr std::size_t kMax = 40;
			if (hits.size() > kMax) {
				hits.resize(kMax);
			}

			for (const auto& h : hits) {
				const std::int32_t token = AllocToken(h.ref);
				json               item = {
					{ "token", token },
					{ "name", ScanLabel(h.ref) },
					{ "formId", h.ref->GetFormID() },
					{ "distance", std::sqrt(h.distSq) / 70.0f },  // game units -> ~meters
					{ "isActor", h.ref->IsActor() },
				};
				if (h.sceneCount >= 0) {
					item["sceneCount"] = h.sceneCount;
				}
				reply["items"].push_back(std::move(item));
			}
			REX::DEBUG("[UI] osf.scanNearby kind={} radius={} -> {} hit(s)", kind, radius, hits.size());
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
				Registry::SceneRegistry::GetSingleton().ForEachDef([&reply, ref](const Registry::SceneDef& d) {
					if (d.RequiresAnchor() && Matchmaking::AnchorAccepts(d, ref)) {
						reply["sceneIds"].push_back(d.id);
					}
				});
			}
			REX::DEBUG("[UI] osf.anchorMatch token={} -> {} scene(s)", token, reply["sceneIds"].size());
			SendJson(a_srcView, "osf.anchorMatch", reply);
		}

		// The view reports each time it becomes visible (ui.visibility -> osf.opened), so the
		// first-run F10 hint can count real opens and retire itself.
		void OnOpened(const char*, const char*, const char*, void*) noexcept
		{
			UI::FirstRunHint::OnMenuOpened();
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

		std::uint32_t mj = 0, mn = 0, pt = 0;
		g_ui->GetPluginVersion(mj, mn, pt);
		REX::INFO("[UI] OSF UI bridge connected (OSF UI v{}.{}.{}, protocol {}) — osf.* commands registered",
			mj, mn, pt, g_ui->GetBridgeProtocolVersion());
	}
}
