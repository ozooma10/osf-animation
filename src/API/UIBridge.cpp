#include "API/UIBridge.h"
#include "API/UIBridgeCatalog.h"
#include "API/UIBridgeWorld.h"

#include "API/OSFSceneAPI.h"  // OSFStartOptions + IOSFSceneAPI + kOSFSceneAPIVersion (in-process launch)
#include "API/OSFUI_API.h"    // the OSF UI bridge surface (JSON text only)
#include "Animation/GraphManager.h"  // browser playback clock inspection + seek
#include "Camera/CameraService.h"  // browse orbit: osf.orbit engages drag-to-look when no scene camera is live
#include "Input/InputService.h"  // osf.opened/closed -> UI-cursor mode for the orbit camera's drag-steer
#include "Matchmaking/Matchmaker.h"  // AnchorAccepts (osf.anchorMatch single-ref check)
#include "Registry/SceneRegistry.h"
#include "Packs/PackReload.h"
#include "Scene/AnchorResolve.h"  // rendered-world reference anchors + in-front-of-player placement
#include "Scene/SceneInspectionService.h"  // scrub-only playback and preview prop ownership
#include "Scene/SceneRuntime.h"  // ListScenes + SetSceneObserver (the browser's ACTIVE-list push)
#include "Serialization/WheelPins.h"  // ordered animation-wheel customization
#include "UI/HudMessage.h"    // OpenWheel's graceful-degrade popup (OSF UI absent/too old)
#include "Util/Species.h"     // catalog species tag + picked-actor species (creature filtering)
#include "Util/StringUtil.h"  // Util::ToLower

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>
#ifdef ERROR
#	undef ERROR
#endif
#include <format>
#include <limits>
#include <optional>
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
		using UIBridgeCatalog::BuildImportTextReport;
		using UIBridgeCatalog::BuildCatalog;
		using UIBridgeCatalog::BuildFileReport;
		using UIBridgeCatalog::BuildWheelData;
		using UIBridgeCatalog::IsWheelEntryEligible;
		using UIBridgeWorld::AllocToken;
		using UIBridgeWorld::CrosshairRef;
		using UIBridgeWorld::RefSexTag;
		using UIBridgeWorld::ResolveToken;
		using UIBridgeWorld::ScanLabel;

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

		// The in-space "orbit unavailable" notice fired this browser session (OnOrbit runs per
		// drag-delta batch while the orbit stays disengaged — the view must not be spammed).
		// Reset on osf.closed. Game main thread only, like g_viewVisible.
		bool g_orbitSpaceNoticed = false;

		// OSF Animation's own scene API, fetched lazily on first launch/stop.
		IOSFSceneAPI* g_scene = nullptr;

		// Last scene handle we launched, so an osf.stop with no handle can target it.
		std::int32_t g_lastHandle = 0;
		// Browser pause is reversible without flattening a custom launch speed to 1x.
		std::unordered_map<std::int32_t, float> g_resumeSpeeds;

		// PLAYER-cast runtime scenes launched from the browser console are aborted when
		// the browser closes: once the UI is gone there is no stop surface left.
		// Ordinary NPC-only scenes are deliberately NOT tracked — they outlive the
		// browser (vignettes / machinima; the player can just walk away), and the ACTIVE
		// list on reopen is their stop surface. Every player-affecting mechanism (control
		// lock, camera, fade) is already engine-gated on the player being a participant, so
		// an NPC-only scene can never strand a lock. Wheel launches are NOT tracked either —
		// the wheel closes itself right after a successful pick, and the emote must survive
		// that close. Stale entries are harmless (handles are generational; StopScene on an
		// ended scene returns false) and the list is cleared on every close, so it never
		// outgrows one browser session. The inspection service always destroys browser
		// previews on close. Main thread only.
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

		// Our view's manifest id; the SendToWeb target for pushes that aren't a direct reply (e.g. the catalog we push when the bridge becomes ready).
		constexpr const char* kViewId = "osf.animation/browser";  // qualified "<modId>/<viewName>" (OSF UI api-freeze item 1)

		// The OSF UI release this build was developed and tested against. When the
		// installed host reports an older version, the browser's status line grows an
		// UPDATE badge pointing at the OSF UI Nexus page. Bump alongside any new
		// host feature this file starts depending on.
		// The browser now uses OSF UI's 2.0 helper transport. Keep the advisory
		// version aligned with the shipped view manifest.
		constexpr std::uint32_t kOSFUITested[3] = { 2, 0, 0 };
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

		// Parse an inbound payload without throwing (handlers are noexcept). Returns a
		// discarded value on malformed or oversized input; callers treat it as empty.
		json ParsePayload(const char* a_json)
		{
			constexpr std::size_t kMaxPayloadBytes = 1u << 20;
			if (!a_json) {
				return json::parse("", nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
			}

			std::size_t length = 0;
			while (length <= kMaxPayloadBytes && a_json[length] != '\0') {
				++length;
			}
			if (length > kMaxPayloadBytes) {
				REX::WARN("[UI] refused an inbound payload larger than {} bytes", kMaxPayloadBytes);
				return json::parse("", nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
			}
			return json::parse(a_json, a_json + length, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
		}

		// Every live scene for the view's ACTIVE list: handle, sceneId, stage, whether the
		// player is in the cast, and the cast itself (token + name; the player is token -1,
		// NPCs get pick tokens so the view can badge busy crew members and re-target stops).
		// Main thread only (AllocToken touches g_tokens).
		json BuildActiveScenes()
		{
			auto& rt = Scene::SceneRuntime::GetSingleton();
			auto& gm = Animation::GraphManager::GetSingleton();
			// The view polls this at 10 Hz while the ACTIVE list is up, which is also the game-thread
			// beat a RUNNING preview needs to keep its render-only props in step with its clock.
			Scene::SceneInspectionService::GetSingleton().Tick();
			auto inspections = Scene::SceneInspectionService::GetSingleton().List();
			auto* player = RE::PlayerCharacter::GetSingleton();
			// One serializer for both loops below — preview rows and runtime rows must agree on the
			// shape of cast[].
			const auto buildCast = [player](const std::vector<RE::Actor*>& a_participants, bool& a_hasPlayer) {
				json cast = json::array();
				a_hasPlayer = false;
				for (RE::Actor* a : a_participants) {
					if (!a) {
						continue;
					}
					const bool isPlayer = player && a == static_cast<RE::Actor*>(player);
					a_hasPlayer = a_hasPlayer || isPlayer;
					cast.push_back(json{
						{ "token", isPlayer ? -1 : AllocToken(a) },
						{ "name", isPlayer ? std::string{ "Player" } : ScanLabel(a) },
						{ "player", isPlayer },
					});
				}
				return cast;
			};
			json scenes = json::array();
			for (const auto& s : rt.ListScenes()) {
				bool hasPlayer = false;
				json cast = buildCast(s.participants, hasPlayer);
				json item{
					{ "handle", s.handle },
					{ "sceneId", s.id.empty() ? std::string{ "runtime.files" } : s.id },
					{ "stage", rt.GetStage(s.handle) },
					{ "inspection", false },
					{ "player", hasPlayer },
					{ "cast", std::move(cast) },
				};
				if (!s.participants.empty()) {
					if (const auto playback = gm.GetScenePlayback(s.participants.front())) {
						if (s.id.empty()) {
							item["stage"] = playback->stage;  // ad-hoc plans have no registry node/stage mapping
						}
						item["time"] = playback->time;
						item["duration"] = playback->duration;
						item["speed"] = playback->speed;
					}
				}
				scenes.push_back(std::move(item));
			}
			for (const auto& preview : inspections) {
				bool hasPlayer = false;
				json cast = buildCast(preview.participants, hasPlayer);
				json item{
					{ "handle", preview.handle },
					{ "sceneId", preview.sceneId },
					{ "stage", preview.stage },
					{ "inspection", true },
					{ "player", hasPlayer },
					{ "cast", std::move(cast) },
					{ "speed", preview.playback.speed },  // 0 = paused (the state a preview starts in)
				};
				item["time"] = preview.playback.time;
				item["duration"] = preview.playback.duration;
				scenes.push_back(std::move(item));
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

		std::optional<std::int32_t> Int32Value(const json& a_value)
		{
			if (a_value.is_number_unsigned()) {
				const auto value = a_value.get<std::uint64_t>();
				if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
					return static_cast<std::int32_t>(value);
				}
				return std::nullopt;
			}
			if (a_value.is_number_integer()) {
				const auto value = a_value.get<std::int64_t>();
				if (value >= std::numeric_limits<std::int32_t>::min() &&
					value <= std::numeric_limits<std::int32_t>::max()) {
					return static_cast<std::int32_t>(value);
				}
			}
			return std::nullopt;
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
			if (const auto value = Int32Value(*it)) {
				return (*value == 0 || *value == 1) ? *value : -1;
			}
			return -1;
		}

		// Type- and range-checked reads for inbound payloads. Every command handler here is
		// noexcept, so malformed peer data must degrade to a default instead of escaping.
		float NumOr(const json& a_obj, const char* a_key, float a_def)
		{
			if (!a_obj.is_object()) {
				return a_def;
			}
			const auto it = a_obj.find(a_key);
			if (it == a_obj.end() || !it->is_number()) {
				return a_def;
			}
			const double value = it->get<double>();
			return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max()
			         ? static_cast<float>(value)
			         : a_def;
		}

		std::int32_t IntOr(const json& a_obj, const char* a_key, std::int32_t a_def)
		{
			if (!a_obj.is_object()) {
				return a_def;
			}
			const auto it = a_obj.find(a_key);
			if (it == a_obj.end()) {
				return a_def;
			}
			const auto value = Int32Value(*it);
			return value.value_or(a_def);
		}
		bool BoolOr(const json& a_obj, const char* a_key, bool a_def)
		{
			if (!a_obj.is_object()) {
				return a_def;
			}
			const auto it = a_obj.find(a_key);
			return it != a_obj.end() && it->is_boolean() ? it->get<bool>() : a_def;
		}
		std::string StrOr(const json& a_obj, const char* a_key)
		{
			if (!a_obj.is_object()) {
				return {};
			}
			const auto it = a_obj.find(a_key);
			return it != a_obj.end() && it->is_string() ? it->get<std::string>() : std::string{};
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

		// The per-file import report. Static between ReloadPacks calls like the library, but small
		// and asked for rarely (only while the IMPORTS panel is open), so it is rebuilt per request
		// rather than cached — a cache would just be one more thing to invalidate on reload.
		void OnImportsGet(const char*, const char*, const char* a_srcView, void*) noexcept
		{
			SendJson(a_srcView, "osf.animation.imports.data", BuildFileReport());
		}
		bool CopyUtf8ToClipboard(std::string_view a_text) noexcept
		{
			constexpr std::size_t kMaxClipboardBytes = 1024 * 1024;
			if (a_text.empty() || a_text.size() > kMaxClipboardBytes ||
				a_text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
				return false;
			}
			const auto bytes = static_cast<int>(a_text.size());
			const int chars = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_text.data(), bytes, nullptr, 0);
			if (chars <= 0) {
				return false;
			}
			HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(chars + 1) * sizeof(wchar_t));
			if (!memory) {
				return false;
			}
			auto* wide = static_cast<wchar_t*>(::GlobalLock(memory));
			if (!wide) {
				::GlobalFree(memory);
				return false;
			}
			const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_text.data(), bytes, wide, chars);
			if (written != chars) {
				::GlobalUnlock(memory);
				::GlobalFree(memory);
				return false;
			}
			wide[chars] = L'\0';
			::GlobalUnlock(memory);

			if (!::OpenClipboard(nullptr)) {
				::GlobalFree(memory);
				return false;
			}
			if (!::EmptyClipboard() || !::SetClipboardData(CF_UNICODETEXT, memory)) {
				::CloseClipboard();
				::GlobalFree(memory);
				return false;
			}
			::CloseClipboard();
			return true;
		}

		bool g_importReloading = false;

		void OnImportsReload(const char*, const char*, const char* a_srcView, void*) noexcept
		{
			if (g_importReloading) {
				SendJson(a_srcView, "osf.animation.imports.reloadResult",
					json{ { "ok", false }, { "error", "A pack reload is already running." } });
				return;
			}
			g_importReloading = true;
			const auto begun = std::chrono::steady_clock::now();
			json reply;
			try {
				reply["scenes"] = Packs::ReloadAll();
				reply["report"] = BuildFileReport();
				reply["ok"] = true;
			} catch (const std::exception& e) {
				reply = { { "ok", false }, { "error", std::string{ "Reload failed: " } + e.what() } };
				REX::ERROR("[UI] Imports pack reload failed: {}", e.what());
			} catch (...) {
				reply = { { "ok", false }, { "error", "Reload failed with an unknown exception." } };
				REX::ERROR("[UI] Imports pack reload failed with an unknown exception");
			}
			reply["durationMs"] = std::chrono::duration<float, std::milli>(
				std::chrono::steady_clock::now() - begun).count();
			g_importReloading = false;
			SendJson(a_srcView, "osf.animation.imports.reloadResult", reply);
		}

		void OnImportsCopy(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const json payload = ParsePayload(a_payload);
			const std::string path = payload.is_object() && payload.contains("path") && payload["path"].is_string()
			                           ? payload["path"].get<std::string>()
			                           : std::string{};
			const auto text = BuildImportTextReport(path);
			const bool ok = text && CopyUtf8ToClipboard(*text);
			SendJson(a_srcView, "osf.animation.imports.copyResult", json{
				{ "ok", ok },
				{ "path", path },
				{ "error", ok ? "" : (text ? "Windows could not open the clipboard." : "That import record no longer exists.") },
			});
		}

		struct BrowserLaunchPlan
		{
			std::string                              sceneId;
			bool                                     singleAnimation = false;
			std::vector<RE::Actor*>                  actors;
			RE::TESObjectREFR*                       furniture = nullptr;
			OSFStartOptions                          options{};
			bool                                     inspect = false;
			std::string                              locationMode;
			std::int32_t                             locationToken = 0;
			std::vector<std::string>                 roleNames;
			std::optional<Scene::PreparedInspection> preparedInspection;
		};

		std::optional<std::string> ResolveLaunchCast(const json& a_payload, BrowserLaunchPlan& a_plan)
		{
			if (a_payload.contains("castTokens") && a_payload["castTokens"].is_array()) {
				for (const auto& value : a_payload["castTokens"]) {
					const auto token = Int32Value(value);
					if (!token) {
						return "Malformed cast token";
					}
					RE::TESObjectREFR* ref = ResolveToken(*token);
					if (!ref || !ref->IsActor()) {
						return "A selected cast member is no longer available — re-pick it";
					}
					a_plan.actors.push_back(static_cast<RE::Actor*>(ref));
				}
			}
			if (a_plan.actors.empty()) {
				return "No cast selected";
			}
			return std::nullopt;
		}

		std::optional<std::string> ResolveLaunchFurniture(const json& a_payload, BrowserLaunchPlan& a_plan)
		{
			if (!a_payload.contains("furnitureToken")) {
				return std::nullopt;
			}
			const auto token = Int32Value(a_payload["furnitureToken"]);
			if (!token) {
				return "Malformed furniture token";
			}
			if (*token != 0) {
				a_plan.furniture = ResolveToken(*token);
				if (!a_plan.furniture) {
					return "The furniture target is no longer available — re-pick it";
				}
			}
			return std::nullopt;
		}

		std::optional<std::string> BuildLaunchOptions(const json& a_payload, BrowserLaunchPlan& a_plan)
		{
			json opts = json::object();
			if (a_payload.is_object()) {
				if (const auto it = a_payload.find("opts"); it != a_payload.end() && it->is_object()) {
					opts = *it;
				}
			}

			auto& options = a_plan.options;
			options.stripMode = OptTri(opts, "strip");
			options.lockPlayerMode = OptTri(opts, "lockPlayer");
			options.playerControlMode = OptTri(opts, "playerControl");
			options.fadeMode = OptTri(opts, "fade");
			options.speed = NumOr(opts, "speed", 1.0f);
			a_plan.inspect = !g_wheel.active && BoolOr(a_payload, "inspect", false);
			if (a_plan.inspect) {
				options.speed = 0.0f;
			}

			options.startStage = 0;
			// Resolve a linear browser stage to its node before the scene starts.
			if (const auto it = opts.find("stage"); it != opts.end()) {
				const auto stage = Int32Value(*it);
				if (!stage || *stage < 0) {
					return "Malformed start stage";
				}
				options.startStage = *stage;
			}
			if (const auto it = opts.find("camera"); it != opts.end() && it->is_string()) {
				std::snprintf(options.camera, sizeof(options.camera), "%s", it->get<std::string>().c_str());
			}
			options.anchorRef = a_plan.furniture;

			// The browser's location selector is a bridge-only extension to OSFStartOptions.
			a_plan.locationMode = a_plan.furniture ? "furniture" : "cast";
			if (a_payload.is_object()) {
				if (const auto it = a_payload.find("location"); it != a_payload.end() && it->is_object()) {
					if (const auto mode = it->find("mode"); mode != it->end() && mode->is_string()) {
						a_plan.locationMode = mode->get<std::string>();
					}
					if (const auto token = it->find("token"); token != it->end()) {
						const auto parsed = Int32Value(*token);
						if (!parsed) {
							return "Malformed location token";
						}
						a_plan.locationToken = *parsed;
					}
				}
			}

			if (a_plan.locationMode == "player") {
				options.anchorRef = RE::PlayerCharacter::GetSingleton();
				if (!options.anchorRef) {
					return "The player is not available as a scene location";
				}
			} else if (a_plan.locationMode == "actor") {
				options.anchorRef = ResolveToken(a_plan.locationToken);
				if (!options.anchorRef || !options.anchorRef->IsActor()) {
					return "The selected actor location is no longer available — re-pick it";
				}
			} else if (a_plan.locationMode == "furniture") {
				if (a_plan.locationToken != 0) {
					options.anchorRef = ResolveToken(a_plan.locationToken);
				}
				if (options.anchorRef && options.anchorRef->IsActor()) {
					return "The selected furniture location is an actor — pick furniture or a marker";
				}
				a_plan.furniture = options.anchorRef;
			} else if (a_plan.locationMode == "front") {
				// Starfield world transforms are meters; ten feet is 3.048 m.
				const auto anchor = Scene::MakeAnchorInFrontOfView(RE::PlayerCharacter::GetSingleton(), 3.048f);
				if (!anchor.set) {
					return "The player is not available for front-of-player placement";
				}
				options.anchorRef = nullptr;
				options.hasAnchor = true;
				options.anchorX = anchor.pos.x;
				options.anchorY = anchor.pos.y;
				options.anchorZ = anchor.pos.z;
				options.anchorHeadingRad = anchor.heading;
			} else if (a_plan.locationMode != "cast") {
				return "Unknown scene location mode '" + a_plan.locationMode + "'";
			}
			return std::nullopt;
		}

		void LogLaunchRequest(const BrowserLaunchPlan& a_plan)
		{
			std::string castDiag;
			for (RE::Actor* actor : a_plan.actors) {
				castDiag += std::format("{}{:08X}", castDiag.empty() ? "" : ",", actor->formID);
			}
			REX::DEBUG("[UI] launch request '{}' cast=[{}] location={} token={} activeBefore={}",
				a_plan.sceneId, castDiag, a_plan.locationMode, a_plan.locationToken,
				Scene::SceneRuntime::GetSingleton().ListScenes().size());
		}

		void ApplyWheelLaunchPosture(BrowserLaunchPlan& a_plan)
		{
			if (!g_wheel.active) {
				return;
			}
			// Wheel entries are stage-pinned in-world flourishes: no placement pin,
			// control lock, strip, fade, or authored scene camera.
			auto& options = a_plan.options;
			options.inPlaceMode = 1;
			options.lockPlayerMode = 0;
			options.stripMode = 0;
			options.fadeMode = 0;
			std::snprintf(options.camera, sizeof(options.camera), "none");
		}

		std::optional<std::string> ResolveLaunchRoles(const json& a_payload, BrowserLaunchPlan& a_plan)
		{
			if (a_payload.contains("roleNames") && a_payload["roleNames"].is_array()) {
				for (const auto& value : a_payload["roleNames"]) {
					a_plan.roleNames.push_back(value.is_string() ? value.get<std::string>() : std::string{});
				}
			}
			if (!a_plan.roleNames.empty() && a_plan.roleNames.size() != a_plan.actors.size()) {
				return "Role names do not match the selected cast";
			}
			return std::nullopt;
		}

		std::optional<std::string> PrepareLaunchInspection(
			BrowserLaunchPlan& a_plan, Scene::SceneInspectionService& a_service)
		{
			if (!a_plan.inspect) {
				return std::nullopt;
			}
			auto definition = Registry::SceneRegistry::GetSingleton().Find(a_plan.sceneId);
			if (!definition) {
				return "The selected scene definition is no longer loaded";
			}

			Scene::InspectionAnchor anchor{};
			if (a_plan.options.anchorRef) {
				const auto resolved = Scene::ResolveSceneAnchor(
					a_plan.sceneId, a_plan.options.anchorRef, std::nullopt, /*a_emitHud*/ false);
				if (!resolved) {
					return "The selected scene cannot use that anchor";
				}
				anchor = Scene::InspectionAnchor{ resolved->set, resolved->pos, resolved->heading };
			} else if (definition->RequiresAnchor()) {
				return "This scene requires compatible furniture";
			} else if (a_plan.options.hasAnchor) {
				anchor = Scene::InspectionAnchor{
					true,
					RE::NiPoint3{
						a_plan.options.anchorX,
						a_plan.options.anchorY,
						a_plan.options.anchorZ,
					},
					a_plan.options.anchorHeadingRad,
				};
			}

			std::string prepareError;
			a_plan.preparedInspection = a_service.Prepare(
				Scene::InspectionRequest{
					definition,
					a_plan.actors,
					a_plan.roleNames,
					a_plan.options.startStage,
					anchor,
				},
				prepareError);
			return a_plan.preparedInspection ? std::nullopt : std::optional<std::string>{ std::move(prepareError) };
		}

		void SupersedeLaunchActors(const BrowserLaunchPlan& a_plan,
			Scene::SceneInspectionService& a_inspectionService, IOSFSceneAPI& a_api)
		{
			for (RE::Actor* actor : a_plan.actors) {
				a_inspectionService.StopForActor(actor);
				const std::int32_t busy = a_api.GetSceneForActor(actor);
				if (busy == 0) {
					continue;
				}
				a_api.StopScene(busy);
				std::erase(g_closeStops, busy);
				g_resumeSpeeds.erase(busy);
				if (busy == g_lastHandle) {
					g_lastHandle = 0;
				}
				REX::DEBUG("[UI] osf.animation.launch '{}' superseding live scene {:#010x} (cast busy) — stopped it first",
					a_plan.sceneId, busy);
			}
		}

		struct BrowserLaunchResult
		{
			std::int32_t               handle = 0;
			std::optional<std::string> error;
		};

		BrowserLaunchResult StartBrowserLaunch(BrowserLaunchPlan& a_plan,
			Scene::SceneInspectionService& a_inspectionService, IOSFSceneAPI& a_api)
		{
			if (a_plan.inspect) {
				std::string startError;
				const auto handle =
					a_inspectionService.Start(std::move(*a_plan.preparedInspection), startError);
				if (handle == 0) {
					return { handle, std::move(startError) };
				}
				return { handle, std::nullopt };
			}
			if (!a_plan.roleNames.empty()) {
				std::vector<const char*> rolePtrs;
				rolePtrs.reserve(a_plan.roleNames.size());
				for (const auto& role : a_plan.roleNames) {
					rolePtrs.push_back(role.c_str());
				}
				return {
					a_api.StartSceneRoles(
						a_plan.actors.data(),
						static_cast<std::uint32_t>(a_plan.actors.size()),
						a_plan.sceneId.c_str(),
						rolePtrs.data(),
						static_cast<std::uint32_t>(rolePtrs.size()),
						a_plan.options),
					std::nullopt,
				};
			}
			return {
				a_api.StartScene(
					a_plan.actors.data(),
					static_cast<std::uint32_t>(a_plan.actors.size()),
					a_plan.sceneId.c_str(),
					a_plan.options),
				std::nullopt,
			};
		}

		bool RecordLaunchSuccess(const BrowserLaunchPlan& a_plan, std::int32_t a_handle)
		{
			g_lastHandle = a_handle;
			// A stage-scoped browser row and every wheel entry mean "play this
			// animation", not "continue through the parent collection".
			if (!a_plan.inspect && (g_wheel.active || a_plan.singleAnimation)) {
				Scene::SceneRuntime::GetSingleton().SetSingleStage(a_handle);
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			const bool castHasPlayer = player &&
				std::find(a_plan.actors.begin(), a_plan.actors.end(), static_cast<RE::Actor*>(player)) !=
					a_plan.actors.end();
			if (!a_plan.inspect && !g_wheel.active && castHasPlayer) {
				g_closeStops.push_back(a_handle);
			}
			return castHasPlayer;
		}

		void OnLaunch(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const json j = ParsePayload(a_payload);
			BrowserLaunchPlan plan;
			plan.sceneId = (j.is_object() && j.contains("sceneId") && j["sceneId"].is_string())
			                 ? j["sceneId"].get<std::string>()
			                 : std::string{};
			plan.singleAnimation = BoolOr(j, "singleAnimation", false);

			json reply;
			reply["sceneId"] = plan.sceneId;
			const auto fail = [&](const std::string& a_reason) {
				reply["ok"] = false;
				reply["handle"] = 0;
				reply["error"] = a_reason;
				REX::WARN("[UI] osf.animation.launch '{}' refused: {}", plan.sceneId, a_reason);
				SendJson(a_srcView, "osf.animation.launchResult", reply);
			};

			if (plan.sceneId.empty()) {
				return fail("No scene selected");
			}
			if (const auto error = ResolveLaunchCast(j, plan)) {
				return fail(*error);
			}
			if (const auto error = ResolveLaunchFurniture(j, plan)) {
				return fail(*error);
			}
			if (const auto error = BuildLaunchOptions(j, plan)) {
				return fail(*error);
			}

			LogLaunchRequest(plan);
			ApplyWheelLaunchPosture(plan);
			if (const auto error = ResolveLaunchRoles(j, plan)) {
				return fail(*error);
			}

			auto& inspectionService = Scene::SceneInspectionService::GetSingleton();
			if (const auto error = PrepareLaunchInspection(plan, inspectionService)) {
				return fail(*error);
			}
			auto* api = SceneAPI();
			if (!api) {
				return fail("OSF Animation engine is not ready yet");
			}

			SupersedeLaunchActors(plan, inspectionService, *api);
			BrowserLaunchResult started = StartBrowserLaunch(plan, inspectionService, *api);
			if (started.handle == 0) {
				return fail(started.error
				                ? *started.error
				                : LaunchError(plan.sceneId, plan.actors.size(), plan.furniture != nullptr));
			}

			const bool castHasPlayer = RecordLaunchSuccess(plan, started.handle);
			reply["ok"] = true;
			reply["handle"] = started.handle;
			reply["inspect"] = plan.inspect;
			REX::DEBUG("[UI] osf.animation.launch '{}' -> handle {} ({} cast{}{})",
				plan.sceneId, started.handle, plan.actors.size(),
				plan.furniture ? ", anchored" : "",
				castHasPlayer ? "" : ", NPC-only — outlives the browser");
			SendJson(a_srcView, "osf.animation.launchResult", reply);
			PushActiveScenes();
		}

		std::optional<std::int32_t> CommandHandle(const char* a_payload)
		{
			const json j = ParsePayload(a_payload);
			std::int32_t handle = 0;
			if (j.is_object()) {
				if (const auto it = j.find("handle"); it != j.end()) {
					const auto parsed = Int32Value(*it);
					if (!parsed) {
						REX::WARN("[UI] scene command refused an invalid/out-of-range handle");
						return std::nullopt;
					}
					handle = *parsed;
				}
			}
			if (handle == 0) {
				handle = g_lastHandle;
			}
			return handle;
		}

		void OnStop(const char*, const char* a_payload, const char*, void*) noexcept
		{
			const auto parsed = CommandHandle(a_payload);
			if (!parsed) {
				return;
			}
			const auto handle = *parsed;
			bool ok = Scene::SceneInspectionService::GetSingleton().Stop(handle);
			if (!ok) {
				if (auto* api = SceneAPI(); api && handle != 0) {
					ok = api->StopScene(handle);
				}
			}
			if (ok) {
				std::erase(g_closeStops, handle);
				g_resumeSpeeds.erase(handle);
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
			const auto parsed = CommandHandle(a_payload);
			if (!parsed) {
				return;
			}
			const auto handle = *parsed;
			bool ok = false;
			if (!Scene::SceneInspectionService::GetSingleton().Contains(handle)) {
				if (auto* api = SceneAPI(); api && handle != 0) {
					ok = api->Advance(handle);
				}
			}
			// A paused runtime scene stays paused across a synchronous NEXT transition.
			// Auto transitions cannot race this path because their clock is already stopped.
			if (ok && g_resumeSpeeds.contains(handle)) {
				auto actors = Scene::SceneRuntime::GetSingleton().GetParticipants(handle);
				if (!actors.empty() && actors.front()) {
					Animation::GraphManager::GetSingleton().SetSpeed(actors.front(), 0.0f);
				}
			}
			REX::DEBUG("[UI] osf.animation.advance handle={} -> {}", handle, ok);
		}

		void OnPlaybackGet(const char*, const char*, const char* a_srcView, void*) noexcept
		{
			SendJson(a_srcView, "osf.animation.activeScenes", BuildActiveScenes());
		}

		void OnPlaybackSet(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const json j = ParsePayload(a_payload);
			if (!j.is_object()) {
				return;
			}
			const auto handle = IntOr(j, "handle", 0);
			auto& inspectionService = Scene::SceneInspectionService::GetSingleton();
			if (inspectionService.Contains(handle)) {
				if (const auto time = j.find("time"); time != j.end() && time->is_number()) {
					const double value = time->get<double>();
					if (std::isfinite(value) && value >= 0.0 && value <= std::numeric_limits<float>::max() &&
						!inspectionService.Seek(handle, static_cast<float>(value))) {
						REX::WARN("[UI] preview seek failed for stale handle={}", handle);
					}
				}
				// Preview transport. A preview carries no timers, loop targets, or authored marks, so
				// running one simply loops its clip with the render-only props reconciled per poll —
				// it still fires no cues, no sounds, and no scene actions.
				if (const auto paused = j.find("paused"); paused != j.end() && paused->is_boolean()) {
					if (!inspectionService.SetSpeed(handle, paused->get<bool>() ? 0.0f : 1.0f)) {
						REX::WARN("[UI] preview transport failed for stale handle={}", handle);
					}
				}
				SendJson(a_srcView, "osf.animation.activeScenes", BuildActiveScenes());
				return;
			}
			auto participants = Scene::SceneRuntime::GetSingleton().GetParticipants(handle);
			if (handle == 0 || participants.empty() || !participants.front()) {
				return;
			}

			auto& gm = Animation::GraphManager::GetSingleton();
			if (const auto paused = j.find("paused"); paused != j.end() && paused->is_boolean()) {
				if (paused->get<bool>()) {
					const float speed = gm.GetSpeed(participants.front());
					if (speed > 0.0f) {
						g_resumeSpeeds[handle] = speed;
					}
					gm.SetSpeed(participants.front(), 0.0f);
				} else {
					const auto found = g_resumeSpeeds.find(handle);
					gm.SetSpeed(participants.front(), found != g_resumeSpeeds.end() ? found->second : 1.0f);
					g_resumeSpeeds.erase(handle);
				}
			}
			SendJson(a_srcView, "osf.animation.activeScenes", BuildActiveScenes());
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
			if (BoolOr(j, "reset", false)) {
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
					const auto stage = Int32Value(*stit);
					if (!stage) {
						REX::WARN("[UI] osf.animation.wheel.set refused a non-integer/out-of-range stage");
						return;
					}
					entry.stage = *stage;
				}

				const auto def = Registry::SceneRegistry::GetSingleton().Find(entry.scene);
				const bool eligible = def && IsWheelEntryEligible(*def, entry.stage);
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

		// Which anchor-bound scenes accept a keyed furniture ref. The view filters its browse
		// list with this: free-space scenes always play; anchor-bound ones only via a match.
		void OnAnchorMatch(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const json   j = ParsePayload(a_payload);
			std::int32_t token = 0;
			if (j.is_object()) {
				if (const auto it = j.find("token"); it != j.end() && it->is_number_integer()) {
					token = Int32Value(*it).value_or(0);
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
			UIBridgeWorld::ClearSessionTokens();  // picked refs are scoped to one browser session
			g_orbitSpaceNoticed = false;  // re-arm the in-space orbit notice for the next session
			Scene::SceneInspectionService::GetSingleton().StopAll();
			// A scrub pauses playback. NPC-only scenes survive the browser, so restore their
			// pre-scrub speeds before their only director surface disappears.
			for (const auto& [handle, speed] : g_resumeSpeeds) {
				auto actors = Scene::SceneRuntime::GetSingleton().GetParticipants(handle);
				if (!actors.empty() && actors.front()) {
					Animation::GraphManager::GetSingleton().SetSpeed(actors.front(), speed);
				}
			}
			g_resumeSpeeds.clear();
			// Abort console-launched PLAYER scenes (see g_closeStops): the browser was the only
			// stop button, so one outliving it would leave the player stuck. NPC-only scenes
			// are not in the list — they keep running until stopped from a reopened browser.
			if (!g_closeStops.empty()) {
				if (auto* api = SceneAPI()) {
					for (const std::int32_t h : g_closeStops) {
						if (api->StopScene(h)) {
							REX::DEBUG("[UI] browser closed — aborted live scene {:#010x}", h);
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
				NumOr(p, "dx", 0.0f), NumOr(p, "dy", 0.0f), NumOr(p, "wheel", 0.0f));
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
			REX::DEBUG("[UI] OpenBrowser: RequestMenu('{}', open) -> {}", kViewId, ok);
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
			REX::DEBUG("[UI] OpenWheel: RequestMenu('{}', open) -> {} (prefix '{}', target: {})",
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
		g_ui.RegisterCommand("osf.animation.imports.get", &OnImportsGet, nullptr);
		g_ui.RegisterCommand("osf.animation.imports.reload", &OnImportsReload, nullptr);
		g_ui.RegisterCommand("osf.animation.imports.copy", &OnImportsCopy, nullptr);
		UIBridgeWorld::RegisterCommands(g_ui);
		g_ui.RegisterCommand("osf.animation.anchorMatch", &OnAnchorMatch, nullptr);
		g_ui.RegisterCommand("osf.animation.launch", &OnLaunch, nullptr);
		g_ui.RegisterCommand("osf.animation.stop", &OnStop, nullptr);
		g_ui.RegisterCommand("osf.animation.advance", &OnAdvance, nullptr);
		g_ui.RegisterCommand("osf.animation.playback.get", &OnPlaybackGet, nullptr);
		g_ui.RegisterCommand("osf.animation.playback.set", &OnPlaybackSet, nullptr);
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
