#include "API/UIBridge.h"

#include "API/OSFSceneAPI.h"  // OSFStartOptions + IOSFSceneAPI + kOSFSceneAPIVersion (in-process launch)
#include "API/OSFUI_API.h"    // the OSF UI bridge surface (JSON text only)
#include "Registry/SceneRegistry.h"
#include "Util/StringUtil.h"  // Util::ToLower

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

// The in-process handle to OSF Animation's own exported scene API. Declared (not
// dllexport here) so we resolve the definition in API/OSFSceneAPI.cpp and reuse
// the exact validated start path other native consumers get.
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

		// token -> picked ref. All handlers run on the GAME MAIN THREAD (CommandFn
		// contract), and the token map is only ever touched from a handler, so no
		// locking is needed. token -1 is reserved for the player (never stored).
		struct Picked
		{
			RE::TESObjectREFR* ref;     // the pointer resolved AT PICK TIME (main thread)
			RE::TESFormID      formID;  // re-validated at use: LookupByID must still == ref
			bool               isActor;
		};
		std::unordered_map<std::int32_t, Picked> g_tokens;
		std::int32_t                             g_nextToken = 1;

		// Our view's manifest id — the SendToWeb target for pushes that aren't a
		// direct reply (e.g. the catalog we push when the bridge becomes ready).
		constexpr const char* kViewId = "osf";

		// ---- helpers ---------------------------------------------------------

		IOSFSceneAPI* SceneAPI()
		{
			if (!g_scene) {
				g_scene = OSF_RequestSceneAPI(kOSFSceneAPIVersion);
			}
			return g_scene;
		}

		// Serialize a payload to the source view. Uses the replace error handler so a
		// non-UTF-8 game name can never throw out of a noexcept handler.
		void SendJson(const char* a_view, const char* a_type, const json& a_payload)
		{
			if (!g_ui) {
				return;
			}
			const std::string text = a_payload.dump(-1, ' ', false, json::error_handler_t::replace);
			g_ui->SendToWeb(a_view, a_type, text.c_str());
		}

		// Parse an inbound payload without throwing (handlers are noexcept). Returns a
		// discarded value on malformed input; callers treat non-objects as empty.
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

		// Actor count for a card: the declared role count, else the first playable
		// stage's clip count (anonymous positional scenes have no roles[]). Reads only
		// the def, so it is safe under the registry read lock (ForEachDef).
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

		// Re-resolve a token to a still-live ref on the main thread. token -1 = player.
		// Guards against unload / formID reuse: the id must still resolve to the very
		// same form we stored, and it must not be flagged deleted.
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

		// Serialize the live scene registry to the osf.catalog.data array. Copies the
		// fields out from under the registry read lock, then builds JSON afterwards —
		// the ForEachDef callback must NOT re-enter the registry.
		json BuildCatalog()
		{
			struct Card
			{
				std::string              id;
				std::string              title;
				std::vector<std::string> tags;
				std::uint32_t            actorCount = 0;
				std::vector<std::string> genders;
				bool                     requiresFurniture = false;
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
				cards.push_back(std::move(c));
			});

			std::sort(cards.begin(), cards.end(), [](const Card& a, const Card& b) {
				const auto la = Util::ToLower(a.title), lb = Util::ToLower(b.title);
				return la != lb ? la < lb : a.id < b.id;
			});

			json arr = json::array();
			for (const auto& c : cards) {
				arr.push_back({
					{ "id", c.id },
					{ "title", c.title },
					{ "tags", c.tags },
					{ "actorCount", c.actorCount },
					{ "genders", c.genders },
					{ "requiresFurniture", c.requiresFurniture },
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

			// commandTarget is NOT guaranteed to be an object reference — aiming at terrain
			// or empty space yields the CELL (or nothing). Calling a TESObjectREFR virtual
			// (GetDisplayFullName) on a non-REFR walks the wrong vtable and crashes, so gate
			// on the non-virtual form type first (Is()/IsActor() only read the formType byte).
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
				const std::int32_t token = g_nextToken++;
				const char*        nm = ref->GetDisplayFullName();
				g_tokens[token] = Picked{ ref, ref->GetFormID(), ref->IsActor() };
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

		void OnBridgeReady(void*) noexcept
		{
			// A nativeBridge view is live. Crucially, the view usually loads and asks for
			// the catalog at the MAIN MENU — before OSF Animation registers its commands at
			// kPostDataLoad — so that first request is rejected. This ready callback fires
			// right after we register, so PUSH the catalog now to populate the already-open
			// view without waiting for it to re-ask.
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
		g_ui->RegisterCommand("osf.launch", &OnLaunch, nullptr);
		g_ui->RegisterCommand("osf.stop", &OnStop, nullptr);

		std::uint32_t mj = 0, mn = 0, pt = 0;
		g_ui->GetPluginVersion(mj, mn, pt);
		REX::INFO("[UI] OSF UI bridge connected (OSF UI v{}.{}.{}, protocol {}) — osf.* commands registered",
			mj, mn, pt, g_ui->GetBridgeProtocolVersion());
	}
}
