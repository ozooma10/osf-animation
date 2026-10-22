#include "API/EngineClient.h"
#include "API/BrowserPlacement.h"
#include "API/BrowserInput.h"
#include "Serialization/ClipDurationCache.h"
#include "API/UIBridge.h"
#include "API/BrowserSession.h"
#include "API/BrowserLaunchSequence.h"
#include "API/BrowserJsonTransport.h"
#include "API/UIBridgeCatalog.h"
#include "API/UIBridgeWorld.h"

#include "OSFSceneAPI.h"
#include "OSFUI.h"
#include "Serialization/WheelPins.h"
#include "Util/StringUtil.h"

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
#include <unordered_map>
#include <vector>
#include <spdlog/spdlog.h>


namespace OSF::API
{
	namespace
	{
		using json = nlohmann::json;
		using UIBridgeCatalog::BuildImportTextReport;
		using UIBridgeCatalog::BuildCatalog;
		using UIBridgeCatalog::ActorCountOf;
		using UIBridgeCatalog::BuildFileReport;
		using UIBridgeCatalog::BuildWheelData;
		using UIBridgeCatalog::IsWheelEntryEligible;
		using UIBridgeWorld::AllocToken;
		using UIBridgeWorld::CrosshairRef;
		using UIBridgeWorld::RefSexTag;
		using UIBridgeWorld::ResolveToken;
		using UIBridgeWorld::ScanLabel;

		auto& g_ui = BrowserSession::Views();
		std::uint32_t g_worldGeneration = 1;
		BrowserJsonTransport g_jsonTransport(
			[](const std::string& view, const std::string& type, const std::string& text) {
				return g_ui && g_ui.EmitEvent(view.c_str(), type.c_str(), text.c_str());
			},
			[](std::function<void()> task) { return EngineClient::Post(std::move(task)); },
			[](const std::string& view, const std::string& type, const char* reason) {
				REX::ERROR("[UI] failed '{}' delivery to '{}': {}", type, view, reason);
			});

		// Sent in the version payload; the view falls back to paced input when false.
		bool g_relativePointerRegistered = false;
		bool          g_relativePointerTraceActive = false;
		std::uint32_t g_relativePointerTraceUpdates = 0;
		float         g_relativePointerTraceDx = 0.0f;
		float         g_relativePointerTraceDy = 0.0f;
		float         g_relativePointerTraceWheel = 0.0f;

		// Frame-queue writes; the optional visibility export may be queried from other threads.
		std::atomic<bool> g_viewVisible = false;

		bool g_orbitSpaceNoticed = false;
		bool g_orbitHeld = false;

		IOSFSceneAPI* g_scene = nullptr;

		std::int32_t g_lastHandle = 0;
		std::unordered_map<std::int32_t, float> g_resumeSpeeds;

		// Player-cast browser launches, stopped on close (no stop surface remains). NPC-only and
		// wheel launches deliberately outlive the browser. Main thread only.
		std::vector<std::int32_t> g_closeStops;

		// Replayed on document initialization; cleared by native hide and OpenBrowser.
		struct PendingWheel
		{
			bool         active = false;
			std::string  tagPrefix;
			std::int32_t targetToken = 0;  // 0 = player-only
			std::string  targetName;
		};
		PendingWheel g_wheel;

		// Captured at open: the engine nulls PlayerCharacter+0xF90 (crosshairRef) while any menu
		// is up, so PICK resolves this instead. 0 = nothing under the reticle.
		std::int32_t g_openPickToken = 0;

		constexpr const char* kViewId = "osf.animation/browser";

		IOSFSceneAPI* SceneAPI()
		{
			if (!g_scene) {
				g_scene = RequestSceneAPI();
			}
			return g_scene;
		}

		// `replace` error handler: a non-UTF-8 game name must not throw out of a noexcept handler.
		void SendJson(const char* a_view, const char* a_type, const json& a_payload)
		{
			if (!g_ui) {
				return;
			}
			std::string text = a_payload.dump(-1, ' ', false, json::error_handler_t::replace);
			const auto  bytes = text.size();
			if (!g_jsonTransport.Send(a_view, a_type, std::move(text))) {
				REX::ERROR("[UI] refused '{}' delivery to '{}' ({} bytes)", a_type, a_view, bytes);
			}
		}

		// Non-throwing; returns a discarded value on malformed or oversized input.
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

		// Main thread only (AllocToken). Player is token -1.
		json BuildActiveScenes()
		{
			json scenes = json::array();
			for (auto item : EngineClient::ActiveScenes()) {
				json cast = json::array();
				bool hasPlayer = false;
				for (const auto& id : item["participants"]) {
					auto* actor = RE::TESForm::LookupByID<RE::Actor>(id.get<std::uint32_t>());
					if (!actor) continue;
					const bool player = actor->IsPlayerRef();
					hasPlayer = hasPlayer || player;
					cast.push_back({ { "token", player ? -1 : AllocToken(actor) },
						{ "name", player ? std::string{"Player"} : ScanLabel(actor) }, { "player", player } });
				}
				const auto id = item["id"].get<std::string>();
				item["sceneId"] = id.empty() ? "runtime.files" : id;
				item.erase("id"); item.erase("participants");
				item["player"] = hasPlayer; item["cast"] = std::move(cast);
				scenes.push_back(std::move(item));
			}
			return json{ { "scenes", std::move(scenes) } };
		}

		// Dropped while hidden; osf.opened re-sends. Main thread only.
		void PushActiveScenes()
		{
			if (!g_ui || !g_viewVisible) {
				return;
			}
			SendJson(kViewId, "osf.animation.activeScenes", BuildActiveScenes());
		}

		// target:null = player. Idempotent in the view, so the osf.opened replay may double-deliver.
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

		// true/1 -> 1, false/0 -> 0, else -1 (inherit).
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

		std::string LaunchError(const std::string& a_sceneId, std::size_t a_castCount, bool a_haveFurniture)
		{
			auto&       reg = EngineClient::CatalogData::GetSingleton();
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

		// Command handlers run on the game main thread.
		void OnCatalogGet(const char*, const char*, const char* a_srcView, void*) noexcept
		{
			SendJson(a_srcView, "osf.animation.version", json{
				{ "plugin", SFSE::GetPluginName() },
				// Version::string() would append the unused build field.
				{ "version", std::format("{}.{}.{}", SFSE::GetPluginVersion().major(), SFSE::GetPluginVersion().minor(), SFSE::GetPluginVersion().patch()) },
				{ "relativePointer", g_relativePointerRegistered },
				// The view never scans the player; this is its only sex-badge channel.
				{ "playerSex", RefSexTag(RE::PlayerCharacter::GetSingleton()) },
			});
			SendJson(a_srcView, "osf.animation.catalog.data", BuildCatalog(false));
		}

		// Static after load: the view caches it; catalog updates never re-push it.
		void OnLibraryGet(const char*, const char*, const char* a_srcView, void*) noexcept
		{
			SendJson(a_srcView, "osf.animation.library.data", BuildCatalog(true));
		}

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
				reply["scenes"] = EngineClient::Get().ReloadContent();
				EngineClient::CatalogData::GetSingleton().Invalidate();
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
			bool                                     wheel = false;
			std::vector<RE::Actor*>                  actors;
			std::vector<RE::NiPointer<RE::TESObjectREFR>> pins;
			RE::TESObjectREFR*                       furniture = nullptr;
			OSFStartOptions                          options{};
			std::string                              locationMode;
			std::int32_t                             locationToken = 0;
			std::vector<std::string>                 roleNames;
		};
		BrowserLaunchSequence::Requests g_launchRequests;

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
			const auto vocabularyTri = [&opts](const char* a_canonical, const char* a_legacy) {
				return opts.contains(a_canonical) ? OptTri(opts, a_canonical) : OptTri(opts, a_legacy);
			};
			options.stripMode = vocabularyTri("hideApparel", "strip");
			options.lockPlayerMode = vocabularyTri("playerInputLock", "lockPlayer");
			options.playerControlMode = vocabularyTri("sceneControls", "playerControl");
			options.fadeMode = OptTri(opts, "fade");
			options.speed = NumOr(opts, "speed", 1.0f);

			options.startStage = 0;
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
				// World units are meters; 3.048 m = 10 ft.
				if (!BrowserPlacement::InFrontOfView(RE::PlayerCharacter::GetSingleton(), 3.048f, options)) {
					return "The player is not available for front-of-player placement";
				}
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
				EngineClient::ActiveScenes().size());
		}

		void ApplyWheelLaunchPosture(BrowserLaunchPlan& a_plan)
		{
			if (!a_plan.wheel) {
				return;
			}
			// Wheel emotes are in-world flourishes: no placement pin, input lock, stripping, fade, or scene camera.
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

		std::int32_t StartBrowserLaunch(const BrowserLaunchPlan& a_plan, IOSFSceneAPI& a_api)
		{
			if (!a_plan.roleNames.empty()) {
				std::vector<const char*> rolePtrs;
				rolePtrs.reserve(a_plan.roleNames.size());
				for (const auto& role : a_plan.roleNames) rolePtrs.push_back(role.c_str());
				return a_api.StartSceneRoles(a_plan.actors.data(), static_cast<std::uint32_t>(a_plan.actors.size()),
					a_plan.sceneId.c_str(), rolePtrs.data(), static_cast<std::uint32_t>(rolePtrs.size()), a_plan.options);
			}
			return a_api.StartScene(a_plan.actors.data(), static_cast<std::uint32_t>(a_plan.actors.size()),
				a_plan.sceneId.c_str(), a_plan.options);
		}

		bool RecordLaunchSuccess(const BrowserLaunchPlan& a_plan, std::int32_t a_handle)
		{
			g_lastHandle = a_handle;
			// Stage rows and wheel entries play one animation, not the parent collection.
			if (a_plan.wheel || a_plan.singleAnimation) {
				EngineClient::Get().SetSingleStage(a_handle);
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			const bool castHasPlayer = player &&
				std::find(a_plan.actors.begin(), a_plan.actors.end(), static_cast<RE::Actor*>(player)) !=
					a_plan.actors.end();
			if (!a_plan.wheel && castHasPlayer) {
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
			plan.wheel = g_wheel.active;

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

			auto* api = SceneAPI();
			if (!api) {
				return fail("OSF Animation engine is not ready yet");
			}

			// Retain both request data and references across deferred scene teardown.
			std::vector<std::uint32_t> cast;
			for (auto* actor : plan.actors) {
				plan.pins.emplace_back(actor);
				cast.push_back(actor->GetFormID());
			}
			if (plan.options.anchorRef) plan.pins.emplace_back(plan.options.anchorRef);
			auto pending = std::make_shared<BrowserLaunchPlan>(std::move(plan));
			const std::string sourceView = a_srcView ? a_srcView : "";
			const auto failPending = [pending, sourceView](const char* reason) {
				REX::WARN("[UI] osf.animation.launch '{}' refused: {}", pending->sceneId, reason);
				SendJson(sourceView.c_str(), "osf.animation.launchResult",
					json{ { "sceneId", pending->sceneId }, { "ok", false }, { "handle", 0 }, { "error", reason } });
			};
			BrowserLaunchSequence::Begin(g_launchRequests, std::move(cast), {
				.owners = [pending, api] {
					std::vector<std::int32_t> owners;
					for (auto* actor : pending->actors) owners.push_back(api->GetSceneForActor(actor));
					return owners;
				},
				.stop = [pending, api](std::int32_t busy) {
					if (!api->StopScene(busy)) return false;
					std::erase(g_closeStops, busy);
					g_resumeSpeeds.erase(busy);
					if (busy == g_lastHandle) g_lastHandle = 0;
					REX::DEBUG("[UI] osf.animation.launch '{}' waiting for scene {:#010x} to release its cast", pending->sceneId, busy);
					return true;
				},
				.start = [pending, api, sourceView, failPending] {
					const auto handle = StartBrowserLaunch(*pending, *api);
					if (handle == 0) {
						failPending(LaunchError(pending->sceneId, pending->actors.size(), pending->furniture != nullptr).c_str());
						return;
					}
					const bool castHasPlayer = RecordLaunchSuccess(*pending, handle);
					REX::DEBUG("[UI] osf.animation.launch '{}' -> handle {} ({} cast{}{})",
						pending->sceneId, handle, pending->actors.size(), pending->furniture ? ", anchored" : "",
						castHasPlayer ? "" : ", NPC-only — outlives the browser");
					SendJson(sourceView.c_str(), "osf.animation.launchResult",
						json{ { "sceneId", pending->sceneId }, { "ok", true }, { "handle", handle } });
					PushActiveScenes();
				},
				.fail = failPending,
				.post = [](std::function<void()> task) { BrowserSession::Post(std::move(task)); }
			});
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
			auto* api = SceneAPI();
			const bool ok = api && handle != 0 && api->StopScene(handle);
			if (ok) {
				std::erase(g_closeStops, handle);
				g_resumeSpeeds.erase(handle);
				if (handle == g_lastHandle) {
					g_lastHandle = 0;
				}
			}
			REX::DEBUG("[UI] osf.animation.stop handle={} -> {}", handle, ok);
		}

		// The only advance channel for browser launches: the overlay swallows keyboard input, and
		// NPC-only casts have no director grant. Handle 0 = last launch.
		void OnAdvance(const char*, const char* a_payload, const char*, void*) noexcept
		{
			const auto parsed = CommandHandle(a_payload);
			if (!parsed) {
				return;
			}
			const auto handle = *parsed;
			auto* api = SceneAPI();
			const bool ok = api && handle != 0 && api->Advance(handle);
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
			if (handle == 0) return;
			auto& gm = EngineClient::Get();
			if (gm.GetSceneSpeed(handle) < 0.0f) return;
			if (const auto paused = j.find("paused"); paused != j.end() && paused->is_boolean()) {
				if (paused->get<bool>()) {
					const float speed = gm.GetSceneSpeed(handle);
					if (speed > 0.0f) {
						g_resumeSpeeds[handle] = speed;
					}
					gm.SetSceneSpeed(handle, 0.0f);
				} else {
					const auto found = g_resumeSpeeds.find(handle);
					gm.SetSceneSpeed(handle, found != g_resumeSpeeds.end() ? found->second : 1.0f);
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

		// Payload is the complete ordered loadout; `reset:true` returns to derived defaults.
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

				const auto def = EngineClient::CatalogData::GetSingleton().Find(entry.scene);
				const bool eligible = def && IsWheelEntryEligible(*def, entry.stage);
				if (!eligible) {
					REX::WARN("[UI] osf.animation.wheel.set refused ineligible animation '{}' stage {}", entry.scene, entry.stage);
					return;
				}
				entries.push_back(std::move(entry));
			}
			// The view applied these pins optimistically; re-shipping the whole catalog would also drop its cast match.
			if (Serialization::WheelPins::SetEntries(entries)) {
				REX::DEBUG("[UI] animation wheel customized with {} entr{}", entries.size(), entries.size() == 1 ? "y" : "ies");
			}
		}

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
				reply["sceneIds"] = EngineClient::MatchAnchor(ref);
			}
			REX::DEBUG("[UI] osf.animation.anchorMatch token={} -> {} scene(s)", token, reply["sceneIds"].size());
			SendJson(a_srcView, "osf.animation.anchorMatch", reply);
		}

		// Native so READY matches the real start gate. Tokens are echoed so the view drops stale replies.
		void OnCastMatch(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const json j = ParsePayload(a_payload);
			json reply = {
				{ "tokens", json::array() },
				{ "sceneIds", json::array() },
			};
			std::vector<RE::Actor*> actors;
			bool valid = j.is_object() && j.contains("tokens") && j["tokens"].is_array() && j["tokens"].size() <= 32;
			if (valid) {
				actors.reserve(j["tokens"].size());
				for (const auto& value : j["tokens"]) {
					const auto token = Int32Value(value);
					if (!token) {
						valid = false;
						break;
					}
					reply["tokens"].push_back(*token);
					RE::TESObjectREFR* ref = ResolveToken(*token);
					if (!ref || !ref->IsActor()) {
						valid = false;
						break;
					}
					actors.push_back(static_cast<RE::Actor*>(ref));
				}
			}

			if (valid && !actors.empty()) {
				reply["sceneIds"] = EngineClient::MatchCast(actors);
			}
			REX::DEBUG("[UI] osf.animation.castMatch {} actor(s) -> {} scene(s)", actors.size(), reply["sceneIds"].size());
			SendJson(a_srcView, "osf.animation.castMatch", reply);
		}

		// UI cursor visible: the scene-orbit camera steers by LMB-drag instead of free-look.
		void ReplayBrowser(const char*, const char*, const char* a_srcView, void*) noexcept
		{
			if (!g_viewVisible) return;
			// Replay before any tokens, including when the document missed a reset while hidden.
			SendJson(a_srcView, "osf.animation.world", json{ { "generation", g_worldGeneration } });
			if (g_wheel.active) {
				SendWheelMode();
			}
			PushActiveScenes();
			// Seed the view with the open-time crosshair target; furniture only if some scene accepts it.
			if (g_openPickToken != 0) {
				if (RE::TESObjectREFR* ref = ResolveToken(g_openPickToken)) {
					const bool isActor = ref->IsActor();
					bool       usable = isActor;
					if (!isActor) {
						usable = !EngineClient::MatchAnchor(ref).empty();
					}
					if (usable) {
						json p;
						p["slot"] = isActor ? "actor" : "furniture";
						p["token"] = g_openPickToken;
						const std::string name = ScanLabel(ref);
						const auto identity = isActor ? EngineClient::ActorIdentity(static_cast<RE::Actor*>(ref)) : EngineClient::ActorIdentityInfo{};
						p["name"] = name;
						p["formId"] = ref->GetFormID();
						p["species"] = identity.species;
						p["sex"] = identity.gender;
						REX::DEBUG("[UI] OnOpened: seeding view with crosshair target '{}' as {}", name, p["slot"].get<std::string>());
						SendJson(a_srcView, "osf.animation.openTarget", p);
					}
				}
			}
		}

		void OnShown()
		{
			g_viewVisible = true;
			BrowserInput::SetVisible(EngineClient::Get(), true);
			ReplayBrowser(nullptr, nullptr, kViewId, nullptr);
			const auto* target = ResolveToken(g_openPickToken);
			if (!g_wheel.active && (!target || !target->IsActor())) {
				EngineClient::Get().KickToThirdPerson();
			}
		}

		void OnHidden()
		{
			g_viewVisible = false;
			g_jsonTransport.Clear();
			g_relativePointerTraceActive = false;
			BrowserInput::SetVisible(EngineClient::Get(), false);
			if (g_orbitHeld) EngineClient::Get().ReleaseOrbit();
			g_orbitHeld = false;
			g_wheel = {};
			g_openPickToken = 0;
			// Cast and placement remain in the view across close/reopen. Their tokens live
			// until world replacement; ResolveToken still validates each reference at use.
			g_orbitSpaceNoticed = false;
			// NPC-only scenes outlive the browser; unpause them before their only control surface goes.
			for (const auto& [handle, speed] : g_resumeSpeeds) {
				EngineClient::Get().SetSceneSpeed(handle, speed);
			}
			g_resumeSpeeds.clear();
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

		// Visibility is host-driven, so the view cannot hide itself.
		void OnRequestClose(const char*, const char*, const char*, void*) noexcept
		{
			const bool ok = g_ui.RequestMenu(kViewId, false);
			REX::DEBUG("[UI] osf.animation.requestClose -> RequestMenu('{}', close) -> {}", kViewId, ok);
		}

		bool EnsureBrowseOrbit(const char* a_srcView)
		{
			if (!g_viewVisible) {
				return false;
			}
			// Lazily engage on the first drag; focus the player's live scene cast, else the player.
			if (!g_orbitHeld) {
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
				g_orbitHeld = EngineClient::Get().AcquireOrbit(cast.data(), static_cast<std::uint32_t>(cast.size()));
				if (!g_orbitHeld && !g_orbitSpaceNoticed) {
					// The orbit cannot engage aboard a ship in space.
					g_orbitSpaceNoticed = true;
					json notice;
					notice["kind"] = "info";
					notice["text"] = "Camera orbit is unavailable in space — land to use it.";
					SendJson(a_srcView, "osf.animation.notice", notice);
				}
			}
			return g_orbitHeld;
		}

		// Fallback when relative-pointer registration is unavailable.
		void OnOrbit(const char*, const char* a_payloadJson, const char* a_srcView, void*) noexcept
		{
			const json p = ParsePayload(a_payloadJson);
			if (!p.is_object()) {
				return;
			}
			if (!EnsureBrowseOrbit(a_srcView)) {
				REX::TRACE("[UI] osf.animation.orbit unavailable or after close — dropped");
				return;
			}
			BrowserInput::Orbit(EngineClient::Get(),
				NumOr(p, "dx", 0.0f), NumOr(p, "dy", 0.0f), NumOr(p, "wheel", 0.0f));
		}

		// At most once per game frame. Only kUpdate steers; begin/end/cancel are ownership edges.
		void OnRelativePointer(const char* a_viewId, OSFUI::API::RelativePointerPhase a_phase,
			float a_dx, float a_dy, float a_wheel, void*) noexcept
		{
			if (!a_viewId || std::string_view(a_viewId) != kViewId) {
				return;
			}

			if (a_phase == OSFUI::API::RelativePointerPhase::kBegin) {
				g_relativePointerTraceActive = true;
				g_relativePointerTraceUpdates = 0;
				g_relativePointerTraceDx = 0.0f;
				g_relativePointerTraceDy = 0.0f;
				g_relativePointerTraceWheel = 0.0f;
				REX::DEBUG("[UI] native relative-pointer capture began for '{}'", a_viewId);
				return;
			}

			if (a_phase == OSFUI::API::RelativePointerPhase::kEnd ||
				a_phase == OSFUI::API::RelativePointerPhase::kCancel) {
				REX::DEBUG("[UI] native relative-pointer capture {} after {} update callback(s), total delta ({:.1f}, {:.1f}), wheel {:.1f}",
					a_phase == OSFUI::API::RelativePointerPhase::kEnd ? "ended" : "cancelled",
					g_relativePointerTraceUpdates, g_relativePointerTraceDx,
					g_relativePointerTraceDy, g_relativePointerTraceWheel);
				g_relativePointerTraceActive = false;
				return;
			}

			if (a_phase != OSFUI::API::RelativePointerPhase::kUpdate) {
				return;
			}
			g_relativePointerTraceUpdates++;
			g_relativePointerTraceDx += a_dx;
			g_relativePointerTraceDy += a_dy;
			g_relativePointerTraceWheel += a_wheel;
			const bool orbitReady = EnsureBrowseOrbit(a_viewId);
			if (g_relativePointerTraceUpdates == 1) {
				REX::DEBUG("[UI] native relative-pointer first update dx={:.1f}, dy={:.1f}, wheel={:.1f} (captureBegan={}, browserVisible={}, orbitReady={})",
					a_dx, a_dy, a_wheel, g_relativePointerTraceActive, g_viewVisible.load(), orbitReady);
			}
			if (!orbitReady) {
				REX::TRACE("[UI] native relative-pointer update unavailable or after close — dropped");
				return;
			}
			BrowserInput::Orbit(EngineClient::Get(), a_dx, a_dy, a_wheel);
		}
	}

	void PushCatalogUpdate()
	{
		if (!BrowserSession::Available() || !g_viewVisible) {
			return;  // the next document fetches current data on init
		}
		REX::DEBUG("[UI] clip durations updated — re-pushing catalog to view '{}'", kViewId);
		SendJson(kViewId, "osf.animation.catalog.data", BuildCatalog(false));
	}

	bool BrowserVisible() { return g_viewVisible.load(); }

	bool OpenBrowser()
	{
		if (!BrowserSession::Available()) {
			REX::DEBUG("[UI] OpenBrowser: {}", BrowserSession::UnavailableReason());
			return false;
		}
		// UIMessageQueue is main-thread-only and callers may be on a Papyrus VM thread.
		BrowserSession::Post([]() {
			if (g_wheel.active) {
				g_wheel = {};
				SendJson(kViewId, "osf.animation.mode", json{ { "mode", "browser" } });
			}
			// Must precede the open: the engine nulls crosshairRef while any menu is up.
			g_openPickToken = 0;
			if (RE::TESObjectREFR* ref = CrosshairRef()) {
				g_openPickToken = AllocToken(ref);
				if (spdlog::should_log(spdlog::level::debug)) {
					REX::DEBUG("[UI] OpenBrowser: captured crosshair target '{}' ({:08X}) for PICK", ScanLabel(ref), ref->GetFormID());
				}
			}
			auto* ui = RE::UI::GetSingleton();
			auto* queue = RE::UIMessageQueue::GetSingleton();
			if (ui && queue) {
				for (const char* menu : { "BookMenu", "InventoryMenu", "DataMenu" }) {
					if (ui->IsMenuOpen(menu)) {
						queue->AddMessage(menu, RE::UI_MESSAGE_TYPE::kHide);
						REX::DEBUG("[UI] OpenBrowser: closing '{}' before the browser", menu);
					}
				}
			}
			const bool ok = g_ui.RequestMenu(kViewId, true);
			REX::DEBUG("[UI] OpenBrowser: RequestMenu('{}', open) -> {}", kViewId, ok);
		}, false);
		return true;
	}

	bool OpenWheel(std::string_view a_tagPrefix)
	{
		if (!BrowserSession::Available()) {
			REX::DEBUG("[UI] OpenWheel: {}", BrowserSession::UnavailableReason());
			return false;
		}
		// Settings hotkeys and Papyrus may run on workers.
		BrowserSession::Post([prefix = std::string{ a_tagPrefix }]() {
			g_wheel = {};
			g_wheel.active = true;
			g_wheel.tagPrefix = prefix.empty() ? std::string{ "player.emote." } : prefix;

			// The wheel plays on the target immediately on pick, so gate harder than a browser pick.
			if (RE::TESObjectREFR* ref = CrosshairRef(); ref && ref->IsActor()) {
				auto*       actor = static_cast<RE::Actor*>(ref);
				const char* reject = nullptr;
				if (actor->IsDead()) {
					reject = "dead";
				} else if (actor->combatController != nullptr) {
					// The IsInCombat() vtable slot is unreliable; read the member instead.
					reject = "in combat";
				} else if (EngineClient::ActorSpecies(actor) != "human") {
					reject = "non-human";  // human clips would T-pose them
				}
				if (reject) {
					REX::DEBUG("[UI] OpenWheel: crosshair target '{}' rejected ({}) — player-only wheel", ScanLabel(ref), reject);
				} else {
					g_wheel.targetToken = AllocToken(ref);
					g_wheel.targetName = ScanLabel(ref);
				}
			}

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
			// Covers an already-open view; the osf.opened replay covers a fresh one.
			SendWheelMode();
			const bool ok = g_ui.RequestMenu(kViewId, true);
			REX::DEBUG("[UI] OpenWheel: RequestMenu('{}', open) -> {} (prefix '{}', target: {})",
				kViewId, ok, g_wheel.tagPrefix, g_wheel.targetToken != 0 ? g_wheel.targetName : "player-only");
		}, false);
		return true;
	}

	void InstallUIBridge()
	{
		using namespace OSFUI::API;

		if (!BrowserSession::Connect(OnShown, OnHidden)) {
			return;
		}

		g_relativePointerRegistered = BrowserSession::RegisterPointer(&OnRelativePointer);
		BrowserSession::RegisterSend("osf.animation.catalog.get", &OnCatalogGet, false);
		BrowserSession::RegisterSend("osf.animation.library.get", &OnLibraryGet, false);
		BrowserSession::RegisterSend("osf.animation.imports.get", &OnImportsGet, false);
		BrowserSession::RegisterSend("osf.animation.imports.reload", &OnImportsReload);
		BrowserSession::RegisterSend("osf.animation.imports.copy", &OnImportsCopy);
		UIBridgeWorld::RegisterCommands();
		BrowserSession::RegisterSend("osf.animation.anchorMatch", &OnAnchorMatch);
		BrowserSession::RegisterSend("osf.animation.castMatch", &OnCastMatch);
		BrowserSession::RegisterSend("osf.animation.launch", &OnLaunch);
		BrowserSession::RegisterSend("osf.animation.stop", &OnStop);
		BrowserSession::RegisterSend("osf.animation.advance", &OnAdvance);
		BrowserSession::RegisterSend("osf.animation.playback.get", &OnPlaybackGet);
		BrowserSession::RegisterSend("osf.animation.playback.set", &OnPlaybackSet);
		BrowserSession::RegisterSend("osf.animation.wheel.get", &OnWheelGet, false);
		BrowserSession::RegisterSend("osf.animation.wheel.set", &OnWheelSet);
		BrowserSession::RegisterSend("osf.animation.opened", &ReplayBrowser);
		// Older documents may still report close; native lifecycle owns teardown.
		BrowserSession::RegisterSend("osf.animation.closed", [](const char*, const char*, const char*, void*) noexcept {});
		BrowserSession::RegisterSend("osf.animation.orbit", &OnOrbit);
		BrowserSession::RegisterSend("osf.animation.requestClose", &OnRequestClose);

		const auto subscription = EngineClient::Get().Subscribe([](EngineEvent event, void*) noexcept {
			try {
				if (event == EngineEvent::WorldReset) {
					BrowserSession::Reset([] {
						// The engine revoked mechanisms and scene handles; never restore the old world.
						g_orbitHeld = false;
						g_closeStops.clear(); g_resumeSpeeds.clear(); g_lastHandle = 0;
						OnHidden();
						UIBridgeWorld::ClearWorldTokens();
						++g_worldGeneration;
						SendJson(kViewId, "osf.animation.world", json{ { "generation", g_worldGeneration } });
						g_ui.RequestMenu(kViewId, false);
					});
				} else if (event == EngineEvent::CatalogChanged || event == EngineEvent::ContentReloaded) {
					EngineClient::CatalogData::GetSingleton().Invalidate();
					if (event == EngineEvent::ContentReloaded) {
						EngineClient::Post([] { Browser::ClipDurationCache::Scan(EngineClient::CatalogData::GetSingleton().Clips(), &PushCatalogUpdate); });
					}
					BrowserSession::Post([] { PushCatalogUpdate(); }, false);
				} else {
					BrowserSession::Post([] { PushActiveScenes(); });
				}
			} catch (...) { REX::ERROR("[UI] Could not queue engine notification"); }
		}, nullptr);
		BrowserSession::FinishRegistration(subscription != 0);
		if (!BrowserSession::Available()) return;
		EngineClient::Post([] { Browser::ClipDurationCache::Scan(EngineClient::CatalogData::GetSingleton().Clips(), &PushCatalogUpdate); });
		REX::INFO("[Feature] OSF UI connected (API {}.{}); relative pointer {}",
			g_ui.Version() >> 16, g_ui.Version() & 0xFFFF, g_relativePointerRegistered ? "REGISTERED" : "UNAVAILABLE");
	}
}
