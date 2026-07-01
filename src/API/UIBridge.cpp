#include "API/UIBridge.h"

#include "API/OSFSceneAPI.h"  // OSFStartOptions + IOSFSceneAPI + kOSFSceneAPIVersion (in-process launch)
#include "API/OSFUI_API.h"    // the OSF UI bridge surface (JSON text only)
#include "Matchmaking/Matchmaker.h"  // AnchorAccepts (usable-furniture filter for Scan Nearby)
#include "Registry/SceneRegistry.h"
#include "Util/StringUtil.h"  // Util::ToLower

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
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

		// Serialize the live scene registry to the osf.catalog.data array. Copies the fields out from under the registry read lock, then builds JSON afterwards
		json BuildCatalog()
		{
			struct StageCard
			{
				std::int32_t             index = 0;
				std::string              name;   // stage label ("" = unlabeled)
				std::vector<std::string> tags;
				std::int32_t             clipCount = 0;
				std::string              sig;    // clip-set signature (files joined) for de-dup
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
			};
			std::vector<Card> cards;
			Registry::SceneRegistry::GetSingleton().ForEachDef([&cards](const Registry::SceneDef& d) {
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
					c.stages.push_back(std::move(sc));
				}
				cards.push_back(std::move(c));
			});

			std::sort(cards.begin(), cards.end(), [](const Card& a, const Card& b) {
				const auto la = Util::ToLower(a.title), lb = Util::ToLower(b.title);
				return la != lb ? la < lb : a.id < b.id;
			});

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
				});
			}
			REX::DEBUG("[UI] catalog built -> {} scene(s)", cards.size());
			return arr;
		}

		void OnCatalogGet(const char*, const char*, const char* a_srcView, void*) noexcept
		{
			SendJson(a_srcView, "osf.catalog.data", BuildCatalog());
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
			} else {
				RE::TESObjectCELL* cell = player->parentCell;
				if (cell && cell->IsAttached()) {
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

					// Walk the array ourselves rather than via TESObjectCELL::ForEachReference so / we can null-check the backing stor
					//  an attached cell should never expose size>0 over a null data pointer, but the header's loop would access-violate f it did, and this call is user-triggered at arbitrary moments (mid-load).
					const RE::BSAutoReadLock locker(cell->lock);
					const auto&              refs = cell->references;
					const std::size_t        count = refs.size();
					const auto* const        slots = refs.data();
					for (std::size_t i = 0; slots && i < count; ++i) {
						RE::TESObjectREFR* ref = slots[i].get();
						if (!ref || ref->IsPlayerRef() || ref->IsDeleted()) {
							continue;
						}
						bool ok = false;
						for (const auto* d : anchorDefs) {
							if (Matchmaking::AnchorAccepts(*d, ref)) {
								ok = true;
								break;
							}
						}
						if (!ok) {
							continue;
						}
						const float distSq = origin.GetSquaredDistance(ref->GetPosition());
						if (distSq <= radiusSq) {
							hits.push_back({ ref, distSq });
						}
					}
				}
			}

			std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.distSq < b.distSq; });
			constexpr std::size_t kMax = 40;
			if (hits.size() > kMax) {
				hits.resize(kMax);
			}

			for (const auto& h : hits) {
				const std::int32_t token = AllocToken(h.ref);
				const char*        nm = h.ref->GetDisplayFullName();
				reply["items"].push_back({
					{ "token", token },
					{ "name", (nm && nm[0]) ? nm : "(unnamed)" },
					{ "formId", h.ref->GetFormID() },
					{ "distance", std::sqrt(h.distSq) / 70.0f },  // game units -> ~meters
					{ "isActor", h.ref->IsActor() },
				});
			}
			REX::DEBUG("[UI] osf.scanNearby kind={} radius={} -> {} hit(s)", kind, radius, hits.size());
			SendJson(a_srcView, "osf.scanResults", reply);
		}

		void OnBridgeReady(void*) noexcept
		{
			REX::DEBUG("[UI] OSF UI bridge ready — pushing catalog to view '{}'", kViewId);
			SendJson(kViewId, "osf.catalog.data", BuildCatalog());
		}
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
		g_ui->RegisterCommand("osf.pickCrosshair", &OnPickCrosshair, nullptr);
		g_ui->RegisterCommand("osf.scanNearby", &OnScanNearby, nullptr);
		g_ui->RegisterCommand("osf.launch", &OnLaunch, nullptr);
		g_ui->RegisterCommand("osf.stop", &OnStop, nullptr);

		std::uint32_t mj = 0, mn = 0, pt = 0;
		g_ui->GetPluginVersion(mj, mn, pt);
		REX::INFO("[UI] OSF UI bridge connected (OSF UI v{}.{}.{}, protocol {}) — osf.* commands registered",
			mj, mn, pt, g_ui->GetBridgeProtocolVersion());
	}
}
