#include "API/UIBridge.h"
#include "Util/NativeMainThreadQueue.h"
#include "API/UIBridgeCatalog.h"
#include "API/UIBridgeWorld.h"

#include "API/OSFSceneAPI.h"
#include "API/OSFUI.h"
#include "Animation/GraphManager.h"
#include "Camera/CameraService.h"
#include "Input/InputService.h"
#include "Matchmaking/Matchmaker.h"
#include "Registry/SceneRegistry.h"
#include "Packs/PackReload.h"
#include "Scene/AnchorResolve.h"
#include "Scene/PlaybackPreviewService.h"
#include "Scene/SceneRuntime.h"
#include "Serialization/WheelPins.h"
#include "UI/HudMessage.h"
#include "Util/Species.h"
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

extern "C" OSF::API::IOSFSceneAPI* OSF_RequestSceneAPI(std::uint32_t a_abiVersion);

namespace OSF::API
{
	namespace
	{
		using json = nlohmann::json;
		using UIBridgeCatalog::BuildImportTextReport;
		using UIBridgeCatalog::BuildCatalog;
		using UIBridgeCatalog::ActorCountOf;
		using UIBridgeCatalog::BuildFileReport;
		using UIBridgeCatalog::BuildRoutes;
		using UIBridgeCatalog::BuildWheelData;
		using UIBridgeCatalog::IsWheelEntryEligible;
		using UIBridgeWorld::AllocToken;
		using UIBridgeWorld::CrosshairRef;
		using UIBridgeWorld::RefSexTag;
		using UIBridgeWorld::ResolveToken;
		using UIBridgeWorld::ScanLabel;

		OSFUI::API::Client g_ui;

		// Sent in the version payload; the view falls back to paced input when false.
		bool g_relativePointerRegistered = false;
		bool          g_relativePointerTraceActive = false;
		std::uint32_t g_relativePointerTraceUpdates = 0;
		float         g_relativePointerTraceDx = 0.0f;
		float         g_relativePointerTraceDy = 0.0f;
		float         g_relativePointerTraceWheel = 0.0f;

		// Game main thread only. Gates OnOrbit: a drag flush arriving after osf.closed would
		// otherwise re-engage the orbit on a closed browser and never release it.
		bool g_viewVisible = false;

		bool g_orbitSpaceNoticed = false;

		IOSFSceneAPI* g_scene = nullptr;

		std::int32_t g_lastHandle = 0;
		std::unordered_map<std::int32_t, float> g_resumeSpeeds;

		// Player-cast browser launches, stopped on close (no stop surface remains). NPC-only and
		// wheel launches deliberately outlive the browser. Main thread only.
		std::vector<std::int32_t> g_closeStops;

		// Replayed from osf.opened to survive the open race. Cleared on osf.closed and by OpenBrowser.
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
				g_scene = OSF_RequestSceneAPI(kOSFSceneAPIVersion);
			}
			return g_scene;
		}

		// `replace` error handler: a non-UTF-8 game name must not throw out of a noexcept handler.
		void SendJson(const char* a_view, const char* a_type, const json& a_payload)
		{
			if (!g_ui) {
				return;
			}
			const std::string text = a_payload.dump(-1, ' ', false, json::error_handler_t::replace);
			g_ui.EmitEvent(a_view, a_type, text.c_str());
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
			auto& rt = Scene::SceneRuntime::GetSingleton();
			auto& gm = Animation::GraphManager::GetSingleton();
			auto inspections = Scene::PlaybackPreviewService::GetSingleton().List();
			auto* player = RE::PlayerCharacter::GetSingleton();
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
							item["stage"] = playback->stage;  // ad-hoc plans have no registry stage mapping
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
					{ "speed", preview.playback.speed },  // 0 = paused
				};
				item["time"] = preview.playback.time;
				item["duration"] = preview.playback.duration;
				item["inspectionKind"] = preview.inspectionKind;
				if (!preview.routeId.empty()) item["routeId"] = preview.routeId;
				if (!preview.transitionId.empty()) item["transitionId"] = preview.transitionId;
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
			auto&       reg = Registry::ContentRegistry::GetSingleton();
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

		void OnRoutesGet(const char*, const char*, const char* a_srcView, void*) noexcept
		{
			SendJson(a_srcView, "osf.animation.routes.data", BuildRoutes());
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
				reply["scenes"] = Content::ReloadAll();
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
			std::optional<Scene::PreparedPreview> preparedPreview;
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
			const auto vocabularyTri = [&opts](const char* a_canonical, const char* a_legacy) {
				return opts.contains(a_canonical) ? OptTri(opts, a_canonical) : OptTri(opts, a_legacy);
			};
			options.stripMode = vocabularyTri("hideApparel", "strip");
			options.lockPlayerMode = vocabularyTri("playerInputLock", "lockPlayer");
			options.playerControlMode = vocabularyTri("sceneControls", "playerControl");
			options.fadeMode = OptTri(opts, "fade");
			options.speed = NumOr(opts, "speed", 1.0f);
			a_plan.inspect = !g_wheel.active && BoolOr(a_payload, "inspect", false);
			if (a_plan.inspect) {
				options.speed = 0.0f;
			}

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

		std::optional<std::string> PrepareLaunchPreview(
			BrowserLaunchPlan& a_plan, Scene::PlaybackPreviewService& a_service)
		{
			if (!a_plan.inspect) {
				return std::nullopt;
			}
			auto definition = Registry::ContentRegistry::GetSingleton().Find(a_plan.sceneId);
			if (!definition) {
				return "The selected scene definition is no longer loaded";
			}

			Scene::PreviewWorldAnchor anchor{};
			if (a_plan.options.anchorRef) {
				const auto resolved = Scene::ResolveSceneAnchor(
					a_plan.sceneId, a_plan.options.anchorRef, std::nullopt, /*a_emitHud*/ false);
				if (!resolved) {
					return "The selected scene cannot use that anchor";
				}
				anchor = Scene::PreviewWorldAnchor{ resolved->set, resolved->pos, resolved->heading };
			} else if (definition->RequiresAnchor()) {
				return "This scene requires compatible furniture";
			} else if (a_plan.options.hasAnchor) {
				anchor = Scene::PreviewWorldAnchor{
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
			a_plan.preparedPreview = a_service.Prepare(
				Scene::PreviewRequest{
					definition,
					a_plan.actors,
					a_plan.roleNames,
					a_plan.options.startStage,
					anchor,
				},
				prepareError);
			return a_plan.preparedPreview ? std::nullopt : std::optional<std::string>{ std::move(prepareError) };
		}

		void SupersedeLaunchActors(const BrowserLaunchPlan& a_plan,
			Scene::PlaybackPreviewService& a_previewService, IOSFSceneAPI& a_api)
		{
			for (RE::Actor* actor : a_plan.actors) {
				a_previewService.StopForActor(actor);
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
			Scene::PlaybackPreviewService& a_previewService, IOSFSceneAPI& a_api)
		{
			if (a_plan.inspect) {
				std::string startError;
				const auto handle =
					a_previewService.Start(std::move(*a_plan.preparedPreview), startError);
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
			// Stage rows and wheel entries play one animation, not the parent collection.
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

			auto& previewService = Scene::PlaybackPreviewService::GetSingleton();
			if (const auto error = PrepareLaunchPreview(plan, previewService)) {
				return fail(*error);
			}
			auto* api = SceneAPI();
			if (!api) {
				return fail("OSF Animation engine is not ready yet");
			}

			SupersedeLaunchActors(plan, previewService, *api);
			BrowserLaunchResult started = StartBrowserLaunch(plan, previewService, *api);
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

		void OnRouteInspect(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const json j = ParsePayload(a_payload);
			const std::string routeId = j.is_object() && j.contains("routeId") && j["routeId"].is_string()
				? j["routeId"].get<std::string>() : std::string{};
			const std::string transitionId = j.is_object() && j.contains("transitionId") && j["transitionId"].is_string()
				? j["transitionId"].get<std::string>() : std::string{};
			json reply{
				{ "routeId", routeId },
				{ "transitionId", transitionId },
			};
			const auto fail = [&](std::string_view a_reason) {
				reply["ok"] = false;
				reply["handle"] = 0;
				reply["error"] = a_reason;
				REX::WARN("[UI] route preview '{}:{}' refused: {}", routeId, transitionId, a_reason);
				SendJson(a_srcView, "osf.animation.routeInspectResult", reply);
			};

			if (routeId.empty() || transitionId.empty()) {
				return fail("Choose a route transition to inspect");
			}
			const auto token = j.is_object() && j.contains("actorToken") ? Int32Value(j["actorToken"]) : std::nullopt;
			if (!token) {
				return fail("Choose one route preview actor");
			}
			auto* ref = ResolveToken(*token);
			if (!ref || !ref->IsActor()) {
				return fail("The selected route preview actor is no longer available — re-pick it");
			}
			auto* actor = static_cast<RE::Actor*>(ref);
			auto route = Registry::ContentRegistry::GetSingleton().FindRoute(routeId);
			auto& previewService = Scene::PlaybackPreviewService::GetSingleton();
			std::string error;
			auto prepared = previewService.PrepareRoute(
				Scene::RoutePreviewRequest{ route, transitionId, actor }, error);
			if (!prepared) {
				return fail(error);
			}

			// Replaces any preview or runtime scene on the actor; overlay routes are suspended, not destroyed.
			previewService.StopForActor(actor);
			if (auto* api = SceneAPI()) {
				const auto busy = api->GetSceneForActor(actor);
				if (busy != 0) {
					(void)api->StopScene(busy);
					std::erase(g_closeStops, busy);
					g_resumeSpeeds.erase(busy);
					if (g_lastHandle == busy) g_lastHandle = 0;
				}
			}

			const auto handle = previewService.StartRoute(std::move(*prepared), error);
			if (handle == 0) {
				return fail(error.empty() ? "The route transition preview could not start" : error);
			}
			g_lastHandle = handle;
			reply["ok"] = true;
			reply["handle"] = handle;
			REX::DEBUG("[UI] route preview '{}:{}' -> handle {} actor={:08X}",
				routeId, transitionId, handle, actor->formID);
			SendJson(a_srcView, "osf.animation.routeInspectResult", reply);
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
			bool ok = Scene::PlaybackPreviewService::GetSingleton().Stop(handle);
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

		// The only advance channel for browser launches: the overlay swallows keyboard input, and
		// NPC-only casts have no director grant. Handle 0 = last launch.
		void OnAdvance(const char*, const char* a_payload, const char*, void*) noexcept
		{
			const auto parsed = CommandHandle(a_payload);
			if (!parsed) {
				return;
			}
			const auto handle = *parsed;
			bool ok = false;
			if (!Scene::PlaybackPreviewService::GetSingleton().Contains(handle)) {
				if (auto* api = SceneAPI(); api && handle != 0) {
					ok = api->Advance(handle);
				}
			}
			// Keep a paused scene paused across NEXT.
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
			auto& previewService = Scene::PlaybackPreviewService::GetSingleton();
			if (previewService.Contains(handle)) {
				if (const auto time = j.find("time"); time != j.end() && time->is_number()) {
					const double value = time->get<double>();
					if (std::isfinite(value) && value >= 0.0 && value <= std::numeric_limits<float>::max() &&
						!previewService.Seek(handle, static_cast<float>(value))) {
						REX::WARN("[UI] preview seek failed for stale handle={}", handle);
					}
				}
				// A running preview loops its clip; it never fires cues, sounds, or actions.
				if (const auto paused = j.find("paused"); paused != j.end() && paused->is_boolean()) {
					if (!previewService.SetSpeed(handle, paused->get<bool>() ? 0.0f : 1.0f)) {
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

				const auto def = Registry::ContentRegistry::GetSingleton().Find(entry.scene);
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
				Matchmaking::AnchorMatchCache cache(ref);
				Registry::ContentRegistry::GetSingleton().ForEachDef([&reply, &cache](const Registry::SceneDef& d) {
					if (d.clipsAvailable && d.RequiresAnchor() && cache.Accepts(d)) {
						reply["sceneIds"].push_back(d.id);
					}
				});
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
				Registry::ContentRegistry::GetSingleton().ForEachDef([&reply, &actors](const Registry::SceneDef& d) {
					if (!d.clipsAvailable || ActorCountOf(d) != actors.size()) {
						return;
					}
					const bool accepts = d.roles.empty() || std::ranges::equal(d.roles, actors,
						[](const Registry::SceneRole& role, RE::Actor* actor) {
							return Matchmaking::RoleAccepts(role, actor);
						});
					if (accepts) {
						reply["sceneIds"].push_back(d.id);
					}
				});
			}
			REX::DEBUG("[UI] osf.animation.castMatch {} actor(s) -> {} scene(s)", actors.size(), reply["sceneIds"].size());
			SendJson(a_srcView, "osf.animation.castMatch", reply);
		}

		// UI cursor visible: the scene-orbit camera steers by LMB-drag instead of free-look.
		void OnOpened(const char*, const char*, const char* a_srcView, void*) noexcept
		{
			g_viewVisible = true;
			Input::InputService::GetSingleton().SetUiCursorVisible(true);
			if (g_wheel.active) {
				SendWheelMode();
			}
			PushActiveScenes();
			// Seed the view with the open-time crosshair target; furniture only if some scene accepts it.
			bool actorTargeted = false;
			if (g_openPickToken != 0) {
				if (RE::TESObjectREFR* ref = ResolveToken(g_openPickToken)) {
					const bool isActor = ref->IsActor();
					actorTargeted = isActor;
					bool       usable = isActor;
					if (!isActor) {
						Matchmaking::AnchorMatchCache cache(ref);
						Registry::ContentRegistry::GetSingleton().ForEachDef([&usable, &cache](const Registry::SceneDef& d) {
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
			// Cast defaults to the player, who is invisible in first person. One-shot; not restored.
			if (!g_wheel.active && !actorTargeted) {
				Camera::CameraService::GetSingleton().KickToThirdPerson();
			}
		}

		void OnClosed(const char*, const char*, const char*, void*) noexcept
		{
			g_viewVisible = false;
			Input::InputService::GetSingleton().SetUiCursorVisible(false);
			Camera::CameraService::GetSingleton().ReleaseBrowseOrbit();
			g_wheel = {};
			g_openPickToken = 0;
			UIBridgeWorld::ClearSessionTokens();
			g_orbitSpaceNoticed = false;
			Scene::PlaybackPreviewService::GetSingleton().StopAll();
			// NPC-only scenes outlive the browser; unpause them before their only control surface goes.
			for (const auto& [handle, speed] : g_resumeSpeeds) {
				auto actors = Scene::SceneRuntime::GetSingleton().GetParticipants(handle);
				if (!actors.empty() && actors.front()) {
					Animation::GraphManager::GetSingleton().SetSpeed(actors.front(), speed);
				}
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
					// The orbit cannot engage aboard a ship in space.
					g_orbitSpaceNoticed = true;
					json notice;
					notice["kind"] = "info";
					notice["text"] = "Camera orbit is unavailable in space — land to use it.";
					SendJson(a_srcView, "osf.animation.notice", notice);
				}
			}
			return cam.BrowseOrbitHeld();
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
			Input::InputService::GetSingleton().InjectOrbitDelta(
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
					a_dx, a_dy, a_wheel, g_relativePointerTraceActive, g_viewVisible, orbitReady);
			}
			if (!orbitReady) {
				REX::TRACE("[UI] native relative-pointer update unavailable or after close — dropped");
				return;
			}
			Input::InputService::GetSingleton().InjectOrbitDelta(a_dx, a_dy, a_wheel);
		}
	}

	void PushCatalogUpdate()
	{
		if (!g_ui.IsReady()) {
			return;  // the next document fetches current data on init
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
		// UIMessageQueue is main-thread-only and callers may be on a Papyrus VM thread.
		Util::NativeMainThreadQueue::Post([]() {
			if (g_wheel.active) {
				g_wheel = {};
				SendJson(kViewId, "osf.animation.mode", json{ { "mode", "browser" } });
			}
			// Must precede the open: the engine nulls crosshairRef while any menu is up.
			g_openPickToken = 0;
			if (RE::TESObjectREFR* ref = CrosshairRef()) {
				g_openPickToken = AllocToken(ref);
				REX::DEBUG("[UI] OpenBrowser: captured crosshair target '{}' ({:08X}) for PICK", ScanLabel(ref), ref->GetFormID());
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
		// Settings hotkeys and Papyrus may run on workers.
		Util::NativeMainThreadQueue::Post([prefix = std::string{ a_tagPrefix }]() {
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
				} else if (Util::ActorSpecies(actor) != "human") {
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

		g_relativePointerRegistered = g_ui.RegisterRelativePointer(kViewId, &OnRelativePointer, nullptr);
		g_ui.RegisterSend("osf.animation.catalog.get", &OnCatalogGet, nullptr);
		g_ui.RegisterSend("osf.animation.library.get", &OnLibraryGet, nullptr);
		g_ui.RegisterSend("osf.animation.routes.get", &OnRoutesGet, nullptr);
		g_ui.RegisterSend("osf.animation.imports.get", &OnImportsGet, nullptr);
		g_ui.RegisterSend("osf.animation.imports.reload", &OnImportsReload, nullptr);
		g_ui.RegisterSend("osf.animation.imports.copy", &OnImportsCopy, nullptr);
		UIBridgeWorld::RegisterCommands(g_ui);
		g_ui.RegisterSend("osf.animation.anchorMatch", &OnAnchorMatch, nullptr);
		g_ui.RegisterSend("osf.animation.castMatch", &OnCastMatch, nullptr);
		g_ui.RegisterSend("osf.animation.launch", &OnLaunch, nullptr);
		g_ui.RegisterSend("osf.animation.route.inspect", &OnRouteInspect, nullptr);
		g_ui.RegisterSend("osf.animation.stop", &OnStop, nullptr);
		g_ui.RegisterSend("osf.animation.advance", &OnAdvance, nullptr);
		g_ui.RegisterSend("osf.animation.playback.get", &OnPlaybackGet, nullptr);
		g_ui.RegisterSend("osf.animation.playback.set", &OnPlaybackSet, nullptr);
		g_ui.RegisterSend("osf.animation.wheel.get", &OnWheelGet, nullptr);
		g_ui.RegisterSend("osf.animation.wheel.set", &OnWheelSet, nullptr);
		g_ui.RegisterSend("osf.animation.opened", &OnOpened, nullptr);
		g_ui.RegisterSend("osf.animation.closed", &OnClosed, nullptr);
		g_ui.RegisterSend("osf.animation.orbit", &OnOrbit, nullptr);
		g_ui.RegisterSend("osf.animation.requestClose", &OnRequestClose, nullptr);

		// May fire off-thread (Papyrus StopScene); the hop also lets ReleaseSlot retire the slot first.
		Scene::SceneRuntime::GetSingleton().SetSceneObserver([]() {
			Util::NativeMainThreadQueue::Post([]() { PushActiveScenes(); });
		});

		if (!g_ui.RegisterView(kViewId)) {
			REX::ERROR("[UI] Could not register the animation browser view");
		}
		REX::INFO("[Feature] OSF UI connected (API {}.{}); relative pointer {}",
			g_ui.Version() >> 16, g_ui.Version() & 0xFFFF, g_relativePointerRegistered ? "REGISTERED" : "UNAVAILABLE");
	}
}
