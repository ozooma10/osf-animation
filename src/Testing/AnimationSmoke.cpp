#include "Testing/AnimationSmoke.h"

#ifdef OSF_ANIMATION_SMOKE
#include "Testing/AnimationCases.h"
#include "API/OSFPersistenceAPI.h"
#include "Audio/SoundService.h"
#include "Camera/CameraService.h"
#include "Matchmaking/Matchmaker.h"
#include "Props/PropService.h"
#include "Registry/SoundRegistry.h"
#include "Scene/SceneEventRelay.h"
#include "UI/FadeService.h"
#include "UI/Subtitle.h"
#include "RE/B/BSScriptUtil.h"
#include "API/OSFSceneAPI.h"
#include "API/OSFSettings.h"
#include "API/OSFSettings_Diagnostics.h"
#include "Testing/BrowserProbe.h"

#include "Animation/GraphManager.h"
#include "Registry/ContentRegistry.h"
#include "Scene/SceneRuntime.h"

#include <Windows.h>
#ifdef ERROR
#undef ERROR
#endif
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <format>
#include <stdexcept>
#include <unordered_set>

namespace OSF::Testing::AnimationSmoke
{
	namespace
	{
		using Json = nlohmann::json;
		using Clock = std::chrono::steady_clock;
		constexpr const char* kClip = "meshes/actors/human/animations/common/cower_idle.af";
		struct Event
		{
			std::int32_t handle;
			std::int32_t type;
			std::string node;
			std::string cue;
		};
		struct State
		{
			bool initialized = false;
			bool enabled = false;
			bool running = false;
			bool soloOwned = false;
			bool browserOwned = false;
			std::filesystem::path directory;
			std::string session;
			std::string lastRequest;
			std::unordered_set<std::string> seenRequests;
			Json snapshot{ { "schemaVersion", 1 }, { "runId", "" }, { "state", "idle" }, { "cases", Json::array() } };
			API::IOSFSceneAPI* sceneAPI = nullptr;
			RE::NiPointer<RE::Actor> actor;
			std::int32_t scene = 0;
			std::uint64_t callback = 0;
			std::mutex eventsMutex;
			std::vector<Event> events;
			int phase = 0;
			float pausedTime = 0;
			RE::NiPoint3 anchorPosition{};
			Clock::time_point phaseAt{};
			Clock::time_point deadline{};
			Clock::time_point nextPoll{};
		};
		State g;
		std::atomic<bool> g_worldReset{ false };
		void CleanupFeatures(bool a_worldReset);
		void FailFeature(const char* a_description);

		std::int64_t UtcMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
		}

		bool SafeId(std::string_view a_value)
		{
			return !a_value.empty() && a_value.size() <= 80 &&
				std::ranges::all_of(a_value, [](unsigned char c) {
					return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
						(c >= '0' && c <= '9') || c == '-' || c == '_';
				});
		}

		Json ReadJson(const std::filesystem::path& a_path)
		{
			std::ifstream stream(a_path, std::ios::binary);
			if (!stream) throw std::runtime_error("Cannot open mailbox JSON");
			std::array<char, 65537> bytes{};
			stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
			const auto count = stream.gcount();
			if (count > 65536) throw std::runtime_error("Mailbox JSON exceeds 64 KiB");
			return Json::parse(bytes.data(), bytes.data() + count);
		}

		void WriteJson(const std::filesystem::path& a_path, const Json& a_value)
		{
			auto temporary = a_path;
			temporary += L".tmp";
			{
				std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
				stream << a_value.dump(2);
				stream.flush();
				if (!stream) throw std::runtime_error("Cannot write smoke mailbox JSON");
			}
			if (!MoveFileExW(temporary.c_str(), a_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
				throw std::runtime_error("Cannot publish smoke mailbox JSON");
		}

		void Publish()
		{
			g.snapshot["updatedAtUtcMs"] = UtcMs();
			WriteJson(g.directory / "snapshot.json", g.snapshot);
		}

		void Result(const char* a_name, const char* a_status, std::string a_message)
		{
			if (std::ranges::none_of(kCases,[=](const char* name) { return std::string_view(name) == a_name; }))
				throw std::runtime_error("Assertion name is missing from the compiled inventory");
			for (auto& item : g.snapshot["cases"]) {
				if (item["name"] == a_name) {
					item["status"] = a_status;
					item["message"] = a_message;
					item["detail"] = std::move(a_message);
					break;
				}
			}
			Publish();
		}

		bool OwnsSoloPlayback()
		{
			// No solo generation token: replacement by the identical clip is undetectable.
			return g.soloOwned && g.actor && g.sceneAPI &&
				g.sceneAPI->GetSceneForActor(g.actor.get()) == 0 &&
				(Animation::GraphManager::GetSingleton().GetCurrentAnimation(g.actor.get()) == kClip ||
				 Animation::GraphManager::GetSingleton().GetCurrentAnimation(g.actor.get()).starts_with("OSF/Smoke/"));
		}

		bool ActorUnclaimed(RE::Actor* a_actor)
		{
			return !g.sceneAPI->IsPlaying(a_actor) && g.sceneAPI->GetSceneForActor(a_actor) == 0;
		}

		void Cleanup(bool a_worldReset = false)
		{
			if (g.browserOwned) {
				BrowserProbe::Views().RequestMenu(BrowserProbe::kViewId, false);
				g.browserOwned = false;
			}
			// Solo has no token, so only release the clip while it is still ours.
			if (!a_worldReset) {
				if (g.scene && g.sceneAPI) (void)g.sceneAPI->StopScene(g.scene);
				if (OwnsSoloPlayback()) {
					(void)Animation::GraphManager::GetSingleton().ClearAnchor(g.actor.get());
					(void)g.sceneAPI->SetSpeed(g.actor.get(), 1.0f);
					(void)g.sceneAPI->Stop(g.actor.get());
				}
			}
			CleanupFeatures(a_worldReset);
			if (g.callback && g.sceneAPI) (void)g.sceneAPI->UnregisterSceneEventCallback(g.callback);
			g.callback = 0;
			g.scene = 0;
			g.soloOwned = false;
			g.actor.reset();
		}

		void Finish(const char* a_state, const char* a_reason, bool a_worldReset = false)
		{
			if (std::string_view(a_state) == "passed" && std::ranges::any_of(g.snapshot["cases"],[](const auto& item) { return item["status"] != "passed"; })) {
				a_state="failed"; a_reason="Runtime reached completion with missing or unsuccessful assertions";
			}
			Cleanup(a_worldReset);
			g.running = false;
			g.snapshot["state"] = a_state;
			g.snapshot["message"] = a_reason;
			for (auto& item : g.snapshot["cases"]) {
				if (item["status"] == "pending") {
					item["status"] = "blocked";
					item["message"] = item["detail"] = a_reason;
				}
			}
			Publish();
			REX::DEBUG("[API] smoke run {}: {} ({})", g.snapshot["runId"].get<std::string>(), a_state, a_reason);
		}

		bool Check(const char* a_name, bool a_passed, const char* a_description)
		{
			Result(a_name, a_passed ? "passed" : "failed", a_description);
			if (!a_passed) FailFeature(a_description);
			return a_passed;
		}

		void SceneEvent(const API::OSFSceneEvent* a_event, void*)
		{
			if (!a_event) return;
			std::scoped_lock lock(g.eventsMutex);
			if (g.events.size() < 256)
				g.events.push_back({ a_event->sceneHandle, a_event->eventType,
					a_event->node ? a_event->node : "", a_event->cue ? a_event->cue : "" });
		}

		std::size_t Count(std::int32_t a_handle, std::int32_t a_type, std::string_view a_cue = {})
		{
			std::scoped_lock lock(g.eventsMutex);
			return std::ranges::count_if(g.events, [&](const Event& e) {
				return e.handle == a_handle && e.type == a_type && (a_cue.empty() || e.cue == a_cue);
			});
		}

		API::OSFStartOptions Options()
		{
			API::OSFStartOptions options;
			options.stripMode = options.lockPlayerMode = options.playerControlMode = options.fadeMode = 0;
			options.inPlaceMode = 1;
			std::memcpy(options.camera, "none", 5);
			return options;
		}

		void Phase(int a_phase)
		{
			g.phase = a_phase;
			g.phaseAt = Clock::now();
			g.snapshot["phase"]=a_phase;
		}

		float PhaseSeconds()
		{
			return std::chrono::duration<float>(Clock::now() - g.phaseAt).count();
		}

#include "Testing/AnimationFeatures.inl"

		void Start(const std::string& a_runId)
		{
			if (g.running) throw std::runtime_error("A smoke run is already active");
			if (!SafeId(a_runId)) throw std::runtime_error("Invalid runId");
			g.snapshot = { { "schemaVersion", 1 }, { "runId", a_runId }, { "state", "running" },
				{ "cases", Json::array() }, { "notCovered", {
					"visual pose quality, culling and creature rigs", "physical controller and full launcher/hotkey matrix",
					"audible mix/spatialization and visible fade/subtitle correctness", "weapon draw/sheath restoration",
					"save deletion, failed/cancelled load and restart", "actor 3D unload/reload and all termination causes",
					"all browser editing, wheel assignment and content workflows" } } };
			for (const auto* name : kCases)
				g.snapshot["cases"].push_back({ { "id", name }, { "name", name }, { "status", "pending" }, { "message", "" }, { "detail", "" } });
			g.running = true;
			g_worldReset.store(false);
			g.deadline = Clock::now() + std::chrono::seconds(240);
			g.sceneAPI = API::RequestSceneAPI();
			if (!g.sceneAPI || !g.sceneAPI->IsReady()) {
				Finish("blocked", "Native APIs are not ready");
				return;
			}
			g.actor.reset(RE::PlayerCharacter::GetSingleton());
			if (!g.actor) { Finish("blocked", "A loaded player is required"); return; }
			{
				const auto loaded = g.actor->loadedData.LockRead();
				if (!*loaded || !(*loaded)->data3D) { Finish("blocked", "Player 3D is not loaded"); return; }
			}
			if ((RE::Main::GetSingleton() && RE::Main::GetSingleton()->isGameMenuPaused) ||
				g.sceneAPI->IsPlaying(g.actor.get()) ||
				!Scene::SceneRuntime::GetSingleton().ListScenes().empty()) {
				Finish("blocked", "Close pause menus and stop existing OSF scenes and player playback");
				return;
			}
			if (BrowserProbe::Visible()) { Finish("blocked", "Close the Animation Browser before starting the run"); return; }
			auto& registry = Registry::ContentRegistry::GetSingleton();
			if (!registry.Find("osf.smoke.linear") || !registry.Find("osf.smoke.graph")) {
				Finish("blocked", "Live smoke fixture is not installed or failed registry validation");
				return;
			}
			OSFSettings::API::Client settings;
			OSFSettings::API::Diagnostics::Client diagnostics;
			std::string level;
			if (!Check("integration.settings", settings.Init() && settings.IsReady() &&
				settings.GetEnum("osf.animation.engine", "logLevel", level) == OSFSettings::API::Status::Ok &&
				diagnostics.Init() && diagnostics.Report({ .modId = "osf.animation", .id = "smoke.integration",
					.title = "EXPECTED TEST: Animation diagnostics", .impact = "Transient API acceptance probe" }) == OSFSettings::API::Status::Ok &&
				diagnostics.Clear("osf.animation", "smoke.integration") == OSFSettings::API::Status::Ok,
				"Released Settings values and diagnostics report/clear accepted")) return;
			OSFUI::API::Client views;
			if (!Check("integration.ui", views.Init() && views.Version() == OSFUI::API::kVersion &&
				!OSFUI::API::RequestInterface(0x10000) && BrowserProbe::Available(),
				"Released UI API 2.0 and browser registrations available; visual/input acceptance remains separate")) return;
			if (!feature.browserRegistered) feature.browserRegistered=views.RegisterSend("osf.animation.smoke.observed",BrowserObserved,nullptr);
			if (!feature.browserRegistered) { Finish("blocked","Browser observation endpoint could not register"); return; }
			{ std::lock_guard lock(feature.mutex); feature.browserReport=Json::object(); }
			std::uint32_t major = 0, minor = 0, patch = 0;
			g.sceneAPI->GetPluginVersion(major, minor, patch);
			g.snapshot["pluginVersion"] = std::format("{}.{}.{}", major, minor, patch);
			if (!Check("api.ready", major != 0 && g.sceneAPI->GetInterfaceVersion() == API::kOSFSceneAPIVersion &&
				!API::RequestSceneAPI(0xFFFF0000),
				"Ready API versions, plugin metadata, and incompatible ABI rejection")) return;
			if (!Check("api.invalid-handles", !g.sceneAPI->StopScene(0) && !g.sceneAPI->Advance(0) &&
				!g.sceneAPI->Navigate(0, "missing") && !g.sceneAPI->SetStage(0, 0) &&
				g.sceneAPI->GetSceneStage(0) == -1 && g.sceneAPI->GetSceneParticipants(0, nullptr, 0) == 0 &&
				!g.sceneAPI->UnregisterSceneEventCallback(0),
				"Invalid handles return documented failure sentinels")) return;
			{ std::scoped_lock lock(g.eventsMutex); g.events.clear(); }
			g.callback = g.sceneAPI->RegisterSceneEventCallback(SceneEvent, nullptr);
			if (!g.callback) { Finish("failed", "Native callback registration failed"); return; }
			Phase(0);
		}

		void AdvanceRun()
		{
			if (Clock::now() > g.deadline) { Finish("failed", "240 second runtime deadline elapsed"); return; }
			if (g.phase == 90) { RecoverFeature(); return; }
			if (g.phase >= 100) { AdvanceFeatures(); return; }
			auto* actor = g.actor.get();
			auto& manager = Animation::GraphManager::GetSingleton();
			auto& runtime = Scene::SceneRuntime::GetSingleton();
			const auto options = Options();
			if (!actor || actor != RE::PlayerCharacter::GetSingleton()) {
				Finish("blocked", "Player changed during smoke run", true); return;
			}
			if ((g.phase == 1 || g.phase == 2 || g.phase == 20) && !OwnsSoloPlayback()) {
				g.soloOwned = false;
				Finish("blocked", "Another playback owner displaced the smoke solo clip"); return;
			}
			switch (g.phase) {
			case 0:
				if (!ActorUnclaimed(actor)) { Finish("blocked", "Player became occupied before solo start"); return; }
				g.soloOwned = g.sceneAPI->Play(actor, kClip, "");
				if (!g.soloOwned) { Check("solo.play", false, "Vanilla AF solo playback failed"); return; }
				Phase(1);
				break;
			case 1: {
				const auto playback = manager.GetAnimationPlayback(actor);
				if (PhaseSeconds() < 0.25f) break;
				if (!Check("solo.play", g.sceneAPI->IsPlaying(actor) && playback && playback->time > 0 && playback->duration > 0,
					"Vanilla AF decoded, created a graph, and its clock advanced")) return;
				(void)g.sceneAPI->SetSpeed(actor, 0);
				const auto stopped = manager.GetAnimationPlayback(actor);
				if (!stopped) { Check("solo.speed", false, "Solo graph disappeared before freeze check"); return; }
				g.pausedTime = stopped->time;
				Phase(2);
				break;
			}
			case 2: {
				if (PhaseSeconds() < 0.2f) break;
				const auto playback = manager.GetAnimationPlayback(actor);
				const bool paused = playback && std::abs(playback->time - g.pausedTime) < 0.002f && g.sceneAPI->GetSpeed(actor) == 0;
				const bool resumed = g.sceneAPI->SetSpeed(actor, 1.25f) && std::abs(g.sceneAPI->GetSpeed(actor) - 1.25f) < 0.001f;
				if (!Check("solo.speed", paused && resumed, "Speed zero freezes the clock; speed 1.25 round-trips")) return;
				g.anchorPosition = actor->data.location;
				if (!manager.SetAnchor(actor, g.anchorPosition.x, g.anchorPosition.y, g.anchorPosition.z,
					actor->data.angle.z * 57.2957795f, 0)) {
					Check("solo.anchor", false, "Solo anchor request failed"); return;
				}
				Phase(20);
				break;
			}
			case 20: {
				if (PhaseSeconds() < 0.1f) break;
				const auto pos = actor->data.location;
				const bool retained = std::abs(pos.x - g.anchorPosition.x) < 0.01f &&
					std::abs(pos.y - g.anchorPosition.y) < 0.01f && std::abs(pos.z - g.anchorPosition.z) < 0.01f;
				const bool released = manager.ClearAnchor(actor);
				if (!Check("solo.anchor", retained && released, "Solo anchor retains the current position across frames and releases")) return;
				(void)g.sceneAPI->SetSpeed(actor, 1);
				if (!g.sceneAPI->Stop(actor)) { Check("solo.stop", false, "Solo stop was rejected"); return; }
				Phase(3);
				break;
			}
			case 3:
				if (g.sceneAPI->IsPlaying(actor)) {
					if (!OwnsSoloPlayback()) {
						g.soloOwned = false;
						Finish("blocked", "Another playback owner replaced the fading smoke solo clip"); return;
					}
					if (PhaseSeconds() > 6) Check("solo.stop", false, "Solo fade-out did not remove its graph within 6 seconds");
					break;
				}
				g.soloOwned = false;
				if (!Check("solo.stop", true, "Solo fade-out completed and released the graph")) return;
				if (!ActorUnclaimed(actor)) { Finish("blocked", "Player became occupied before linear scene start"); return; }
				g.scene = g.sceneAPI->StartScene(&actor, 1, "osf.smoke.linear", options);
				if (!Check("scene.start", g.scene != 0 && g.sceneAPI->GetSceneForActor(actor) == g.scene &&
					g.sceneAPI->GetSceneParticipants(g.scene, nullptr, 0) == 1 && g.sceneAPI->GetSceneStage(g.scene) == 0 &&
					Count(g.scene, API::SceneEventType::kSceneBegin) == 1,
					"Registered scene starts with one participant, stage zero, and one begin callback")) return;
				Phase(4);
				break;
			case 4:
				if (!Check("scene.stage", g.sceneAPI->SetStage(g.scene, 1) && g.sceneAPI->GetSceneStage(g.scene) == 1 &&
					!g.sceneAPI->SetStage(g.scene, 99) && g.sceneAPI->GetSceneStage(g.scene) == 1,
					"Stage selection changes the stage and out-of-range requests preserve it")) return;
				if (!g.sceneAPI->Advance(g.scene)) { Check("scene.navigate", false, "Advance from final linear stage failed"); return; }
				Phase(5);
				break;
			case 5:
				if (g.sceneAPI->IsPlaying(actor)) {
					if (PhaseSeconds() > 6) Check("scene.cleanup", false, "Linear scene graph remained after end");
					break;
				}
				if (!ActorUnclaimed(actor)) { Finish("blocked", "Player became occupied before graph scene start"); return; }
				g.scene = g.sceneAPI->StartScene(&actor, 1, "osf.smoke.graph", options);
				if (!Check("scene.navigate", g.scene && !g.sceneAPI->Navigate(g.scene, "missing") &&
					g.sceneAPI->Navigate(g.scene, "next") && runtime.GetNode(g.scene) == "middle" &&
					g.sceneAPI->Advance(g.scene) && runtime.GetNode(g.scene) == "timed",
					"Named navigation and default advance follow graph edges; unknown edge is rejected")) return;
				Phase(6);
				break;
			case 6:
				if (g.sceneAPI->GetSceneForActor(actor) != 0) {
					if (PhaseSeconds() > 6) Check("scene.timer-events", false, "Authored timer did not end the scene within 6 seconds");
					break;
				}
				if (!Check("scene.timer-events", Count(g.scene, API::SceneEventType::kSceneBegin) == 1 &&
					Count(g.scene, API::SceneEventType::kCue, "smoke.tick") == 1 &&
					Count(g.scene, API::SceneEventType::kNodeEnter) == 3 &&
					Count(g.scene, API::SceneEventType::kNodeExit) == 3 && Count(g.scene, API::SceneEventType::kSceneEnd) == 1,
					"Timer ends graph with exactly-once cue and expected begin, node, and end callbacks")) return;
				Phase(7);
				break;
			case 7:
				if (g.sceneAPI->IsPlaying(actor)) {
					if (PhaseSeconds() > 6) Check("scene.cleanup", false, "Scene graph remained after fade-out");
					break;
				}
				if (!Check("scene.cleanup", !g.sceneAPI->StopScene(g.scene) &&
					g.sceneAPI->GetSceneParticipants(g.scene, nullptr, 0) == 1 && g.sceneAPI->GetSceneForActor(actor) == 0 &&
					g.sceneAPI->UnregisterSceneEventCallback(g.callback) && !g.sceneAPI->UnregisterSceneEventCallback(g.callback),
					"Ended scene rejects mutation, retains its roster, frees actor, and callback unregisters once")) return;
				g.callback = 0;
				g.scene = 0;
				g.browserOwned = API::OpenBrowser();
				if (!g.browserOwned) { Check("integration.browser-lifecycle", false, "Browser launch was rejected"); return; }
				Phase(10);
				break;
			case 10:
			case 12:
				if (!BrowserProbe::Visible()) {
					if (PhaseSeconds() > 12) Check("integration.browser-lifecycle", false, "Native browser shown callback did not execute");
					break;
				}
				if (g.phase == 10) {
					const auto page=BrowserReport();
					const bool loaded=page.value("documentReady",false) && page.value("catalog",false) && page.value("library",false) && page.value("imports",false) && page.value("wheel",false);
					if (!WaitFeature("integration.browser-document",loaded,15,"Real document did not render its browser shell and receive all four data feeds")) break;
					if (!Check("integration.browser-document",true,"Actual browser document laid out its shell and received catalog, library, imports and wheel data")) return;
					g.snapshot["browserTarget"]=page.at("target"); ExternalAction("browser-imports"); Phase(14); break;
				}
				// Close through the host, with no JavaScript close notification.
				BrowserProbe::Views().RequestMenu(BrowserProbe::kViewId, false);
				Phase(g.phase + 1);
				break;
			case 11:
			case 13:
				if (BrowserProbe::Visible()) {
					if (PhaseSeconds() > 6) Check("integration.browser-lifecycle", false, "Native browser hidden callback did not execute");
					break;
				}
				if (g.phase == 11) {
					if (!API::OpenBrowser()) { Check("integration.browser-lifecycle", false, "Browser reopen was rejected"); return; }
					Phase(12);
				} else {
					g.browserOwned = false;
					if (!Check("integration.browser-lifecycle", true, "Released UI host drove native shown/hidden cleanup across open, host close, reopen, and close")) return;
					g.browserOwned=BrowserProbe::OpenWheel("");
					if (!g.browserOwned) { Check("integration.wheel",false,"Wheel launch was rejected"); return; }
					Phase(15);
				}
				break;
			case 14: {
				const auto page=BrowserReport();
				if (!WaitFeature("integration.browser-input",page.value("clickedTrusted",false) && page.value("importsOpen",false),15,"Real pointer input did not open the Imports panel")) break;
				if (!Check("integration.browser-input",true,"Owned Windows pointer input reached the real page and opened Imports")) return;
				ExternalAction(""); BrowserProbe::Views().RequestMenu(BrowserProbe::kViewId,false); Phase(11); break;
			}
			case 15: {
				const auto page=BrowserReport();
				if (!WaitFeature("integration.wheel",BrowserProbe::Visible() && page.value("wheelVisible",false) && page.value("wheel",false),15,"Wheel document did not become visible with native wheel data")) break;
				if (!Check("integration.wheel",true,"OpenWheel displayed the real wheel document with its native data")) return;
				BrowserProbe::Views().RequestMenu(BrowserProbe::kViewId,false); Phase(16); break;
			}
			case 16:
				if (!WaitFeature("integration.wheel",!BrowserProbe::Visible(),6,"Wheel failed to close")) break;
				g.browserOwned=false; Phase(100); break;
			}
		}

		void Initialize()
		{
			g.initialized = true;
			const std::filesystem::path configPath = "Data/SFSE/Plugins/OSFAnimationTestHarness.json";
			if (!std::filesystem::exists(configPath)) return;
			const auto config = ReadJson(configPath);
			const auto directoryUtf8 = config.at("stateDir").get<std::string>();
			g.directory = std::filesystem::path(std::u8string(
				reinterpret_cast<const char8_t*>(directoryUtf8.data()), directoryUtf8.size()));
			const auto launchToken = config.at("launchToken").get<std::string>();
			if (!g.directory.is_absolute() || g.directory == g.directory.root_path() || !SafeId(launchToken))
				throw std::runtime_error("Smoke config requires an absolute private stateDir and safe launchToken");
			std::filesystem::create_directories(g.directory / "responses");
			FILETIME created{}, exit{}, kernel{}, user{};
			if (!GetProcessTimes(GetCurrentProcess(), &created, &exit, &kernel, &user))
				throw std::runtime_error("Cannot obtain process creation identity");
			const auto creation = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
			g.session = std::format("{}-{}-{}", GetCurrentProcessId(), creation, UtcMs());
			WriteJson(g.directory / "session.json", { { "pid", GetCurrentProcessId() },
				{ "processStartFileTime", std::to_string(creation) }, { "sessionId", g.session }, { "launchToken", launchToken } });
			Publish();
			g.enabled = true;
			REX::DEBUG("[API] development smoke mailbox ready");
		}

		void PollMailbox()
		{
			if (Clock::now() < g.nextPoll) return;
			g.nextPoll = Clock::now() + std::chrono::milliseconds(100);
			const auto path = g.directory / "request.json";
			if (!std::filesystem::exists(path)) return;
			const auto request = ReadJson(path);
			const auto id = request.value("id", std::string{});
			if (!SafeId(id) || id == g.lastRequest) return;
			g.lastRequest = id;
			Json response{ { "id", id }, { "sessionId", g.session }, { "ok", false }, { "error", "" } };
			try {
				if (request.value("sessionId", std::string{}) != g.session) throw std::runtime_error("Session mismatch");
				const auto expiry = request.at("expiresAtUtcMs").get<std::int64_t>();
				if (expiry <= UtcMs() || expiry > UtcMs() + 300000) throw std::runtime_error("Request expired or deadline exceeds five minutes");
				if (g.seenRequests.contains(id)) throw std::runtime_error("Request ID has already been consumed in this session");
				if (g.seenRequests.size() >= 1024) throw std::runtime_error("Session request limit reached; restart the test process");
				// Never evict: an older unexpired start must not become executable again.
				g.seenRequests.insert(id);
				const auto command = request.at("command").get<std::string>();
				if (command == "start") Start(request.at("args").at("runId").get<std::string>());
				else if (command != "status") throw std::runtime_error("Unknown smoke command");
				response["ok"] = true;
			} catch (const std::exception& e) { response["error"] = e.what(); }
			response["data"] = g.snapshot;
			WriteJson(g.directory / "responses" / (id + ".json"), response);
		}
	}

	void Tick() noexcept
	{
		try {
			if (!g.initialized) Initialize();
			if (!g.enabled) return;
			if (g_worldReset.exchange(false) && g.running) {
				if (g.phase == 172) {
					++feature.resets;
					g.actor.reset(); g.scene = 0; g.soloOwned = false;
				} else Finish("blocked", "Unexpected world replacement interrupted the run", true);
			}
			PollMailbox();
			if (g.running) AdvanceRun();
		} catch (const std::exception& e) {
			REX::ERROR("[API] development smoke bench error: {}", e.what());
			try { if (g.running) Finish("failed", "Smoke runner exception; inspect the plugin log"); }
			catch (...) { Cleanup(); g.running = false; g.enabled = false; }
		} catch (...) {
			REX::ERROR("[API] development smoke bench received an unknown exception");
			g.enabled = false;
		}
	}

	void OnWorldReset() noexcept { g_worldReset.store(true); }
}
#endif
