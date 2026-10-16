#include "Authoring/AlignmentService.h"
#include "Authoring/AlignmentProtocol.h"
#include "Animation/GraphManager.h"
#include "Registry/ContentRegistry.h"
#include "Scene/SceneRuntime.h"
#include "Scene/SceneLauncher.h"
#include "Packs/PackReload.h"
#include "Camera/CameraService.h"
#include "Util/NativeMainThreadQueue.h"

#include <Windows.h>
#ifdef ERROR
#undef ERROR
#endif
#include <array>
#include <chrono>
#include <fstream>
#include <future>
#include <thread>

namespace OSF::Authoring
{
	namespace
	{
		using Json = nlohmann::json;
		using Clock = std::chrono::steady_clock;
		struct Bridge
		{
			std::atomic<bool> active{true};
			std::filesystem::path directory;
			std::string session;
		};
		std::mutex g_control;
		std::shared_ptr<Bridge> g_bridge;
		std::jthread g_worker;

		std::uint64_t UtcMs()
		{
			return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::system_clock::now().time_since_epoch()).count());
		}

		Json Placement(const Animation::ParticipantPlacement& p)
		{
			return {{"x",p.x},{"y",p.y},{"z",p.z},{"heading",p.heading * 57.29577951308232f}};
		}

		Json ReadScenes()
		{
			Json scenes = Json::array();
			auto& runtime = Scene::SceneRuntime::GetSingleton();
			for (const auto& scene : runtime.ListScenes()) {
				if (scene.participants.empty()) continue;
				const auto state = Animation::GraphManager::GetSingleton().ReadAlignment(scene.participants.front(), scene.playbackId);
				if (!state) continue;
				Json actors = Json::array();
				for (const auto& actor : state->participants) {
					actors.push_back({{"formId", actor.formId}, {"role", actor.role}, {"file", actor.file},
						{"authored", Placement(actor.authored)}, {"offset", Placement(actor.offset)},
						{"effective", Placement(Animation::AddPlacement(actor.authored, actor.offset))}});
				}
				std::string source;
				if (const auto def = Registry::ContentRegistry::GetSingleton().Find(scene.id)) source = def->sourceFile.generic_string();
				scenes.push_back({{"target", {{"handle", scene.handle}, {"playback", state->playbackId},
					{"revision", state->playback.segmentRevision}}}, {"id",scene.id}, {"node",scene.node},
					{"linearStage",runtime.GetStage(scene.handle)}, {"segment",state->playback.stage},
					{"sourceFile",source}, {"anchored",state->anchored}, {"time",state->playback.time},
					{"duration",state->playback.duration}, {"speed",state->playback.speed}, {"participants",actors}});
			}
			return scenes;
		}

		Json Execute(const AlignmentRequest& request)
		{
			if (UtcMs() >= request.expiresAtUtcMs) throw std::runtime_error("Command expired before game-thread execution");
			if (request.op == "start") {
				std::vector<RE::Actor*> actors;
				for (auto id : request.actors) {
					auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
					bool loaded = false;
					if (actor) { const auto data = actor->loadedData.LockRead(); loaded = *data && (*data)->data3D; }
					if (!loaded || actor->IsDead()) throw std::runtime_error("Participant is missing, dead or has no loaded 3D: " + std::to_string(id));
					if (Animation::GraphManager::GetSingleton().IsPlaying(actor)) throw std::runtime_error("Participant is already playing; stop its scene first");
					actors.push_back(actor);
				}
				Scene::LaunchOpts opts;
				opts.anchor = RE::TESForm::LookupByID<RE::Actor>(request.anchor);
				opts.hideApparelMode = 0;
				opts.followActorMode = 0;
				opts.camera = "scene_orbit";
				// An explicit authoring launch enters gameplay from the harmless character/pause
				// screens. Do not close inventory, dialogue, confirmation or loading menus.
				if (auto* ui = RE::UI::GetSingleton()) {
					if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
						for (const char* menu : {"PauseMenu", "DataMenu"}) {
							if (ui->IsMenuOpen(menu)) queue->AddMessage(menu, RE::UI_MESSAGE_TYPE::kHide);
						}
					}
				}
				if (!Scene::LaunchDef(request.scene, actors, Scene::MakeDefLaunchOptions(opts), "[API] alignment"))
					throw std::runtime_error("Scene launch refused; inspect the engine log");
			} else if (request.op == "reload") {
				if (!Scene::SceneRuntime::GetSingleton().ListScenes().empty()) throw std::runtime_error("Stop active scenes before authoring reload");
				Content::ReloadAll();
			} else if (request.op != "status") {
				auto& runtime = Scene::SceneRuntime::GetSingleton();
				const auto scenes = runtime.ListScenes();
				const auto found = std::ranges::find_if(scenes, [&](const auto& s) {
					return s.handle == request.handle && s.playbackId == request.playback;
				});
				if (found == scenes.end() || found->participants.empty()) throw std::runtime_error("Scene ended or changed; attach again");
				const auto current = Animation::GraphManager::GetSingleton().ReadAlignment(found->participants.front(), request.playback);
				if (!current || current->playback.segmentRevision != request.revision) throw std::runtime_error("Scene segment changed; attach again");
				if (request.op == "stop") {
					if (!runtime.Stop(request.handle)) throw std::runtime_error("Scene stop refused");
				} else if (request.op == "camera") {
					if (scenes.size() != 1 || !Camera::CameraService::GetSingleton().SetInspectionView(
						request.view[0], request.view[1], request.view[2], request.view[3]))
						throw std::runtime_error("Camera requires one active scene with scene orbit enabled");
				} else {
					auto offsets = request.offsets;
					if (request.op == "reset") offsets.resize(found->participants.size());
					if (!Animation::GraphManager::GetSingleton().UpdateAlignment(found->participants.front(), request.playback,
						request.revision, offsets, request.speed)) {
						throw std::runtime_error("Alignment refused: stale segment, unanchored scene, or invalid participant count; query status");
					}
				}
			}
			return {{"ok",true}, {"scenes",ReadScenes()}};
		}

		void WriteJson(const std::filesystem::path& path, const Json& value)
		{
			auto temporary = path;
			temporary += L".tmp";
			{
				std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
				out << value.dump(2, ' ', false, Json::error_handler_t::replace);
				out.flush();
				if (!out) throw std::runtime_error("Cannot write alignment response");
			}
			const bool published = std::filesystem::exists(path) ?
				ReplaceFileW(path.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr) != 0 :
				MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
			if (!published) throw std::runtime_error("Cannot publish alignment response");
		}

		std::string ReadCommand(const std::filesystem::path& path)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream) return {};
			std::array<char, 65537> bytes{};
			stream.read(bytes.data(), bytes.size());
			if (stream.gcount() > 65536) throw std::runtime_error("Alignment command exceeds 64 KiB");
			return {bytes.data(), static_cast<std::size_t>(stream.gcount())};
		}

		void Worker(std::stop_token stop, const std::shared_ptr<Bridge>& bridge)
		{
			std::uint64_t previousSequence = 0, pendingSequence = 0;
			std::string previousText;
			std::optional<std::future<Json>> pending;
			std::optional<Json> response;
			auto heartbeat = Clock::time_point{};
			auto nextErrorLog = Clock::time_point{};
			while (!stop.stop_requested() && bridge->active) {
				try {
					if (Clock::now() >= heartbeat) {
						WriteJson(bridge->directory / "ready.json", {{"schema",1}, {"enabled",true}, {"session",bridge->session},
							{"pid",GetCurrentProcessId()}, {"updatedAtUtcMs",UtcMs()}});
						heartbeat = Clock::now() + std::chrono::seconds(1);
					}
					if (pending && pending->wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
						response = pending->get();
						pending.reset();
						(*response)["sequence"] = pendingSequence;
					}
					if (response) {
						(*response)["schema"] = 1;
						(*response)["session"] = bridge->session;
						WriteJson(bridge->directory / "response.json", *response);
						response.reset();
					}
					if (!pending) {
						const auto text = ReadCommand(bridge->directory / "command.json");
						if (!text.empty() && text != previousText) {
							previousText = text;
							std::uint64_t sequence = 0;
							try {
								const auto doc = Json::parse(text);
								if (doc.contains("sequence") && doc["sequence"].is_number_unsigned()) sequence = doc["sequence"].get<std::uint64_t>();
								const auto request = ParseAlignmentRequest(doc, bridge->session, previousSequence, UtcMs());
								previousSequence = pendingSequence = request.sequence;
								auto promise = std::make_shared<std::promise<Json>>();
								pending = promise->get_future();
								Util::NativeMainThreadQueue::Post([bridge, promise, request] {
									try {
										if (!bridge->active) throw std::runtime_error("Alignment tools were disabled");
										promise->set_value(Execute(request));
									} catch (const std::exception& error) {
										promise->set_value(Json{{"ok",false}, {"error",error.what()}});
									} catch (...) {
										promise->set_value(Json{{"ok",false}, {"error","Alignment command failed"}});
									}
								});
							} catch (const std::exception& error) {
								response = {{"sequence",sequence}, {"ok",false}, {"error",error.what()}};
							}
						}
					}
				} catch (const std::exception& error) {
					if (Clock::now() >= nextErrorLog) {
						REX::ERROR("[API] alignment file interface: {}", error.what());
						nextErrorLog = Clock::now() + std::chrono::seconds(5);
					}
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			try { WriteJson(bridge->directory / "ready.json", {{"schema",1}, {"enabled",false}, {"session",bridge->session}}); }
			catch (...) {}
		}
	}

	void InitializeAlignmentTools()
	{
		wchar_t value[2]{};
		if (GetEnvironmentVariableW(L"OSF_ANIMATION_ALIGNMENT", value, 2) == 1 && value[0] == L'1') {
			SetAlignmentToolsEnabled(true);
		}
	}

	bool SetAlignmentToolsEnabled(bool a_enabled)
	{
		std::scoped_lock control{g_control};
		try {
			if (a_enabled && g_bridge && g_bridge->active) return true;
			if (g_bridge) g_bridge->active = false;
			if (g_worker.joinable()) { g_worker.request_stop(); g_worker.join(); }
			g_bridge.reset();
			if (!a_enabled) return true;
			const auto logs = SFSE::log::log_directory();
			if (!logs) throw std::runtime_error("SFSE user directory unavailable");
			auto bridge = std::make_shared<Bridge>();
			// CommonLib's log directory is Documents/My Games/Starfield/SFSE/Logs.
			bridge->directory = logs->parent_path().parent_path() / "OSF" / "Alignment";
			std::filesystem::create_directories(bridge->directory);
			bridge->session = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(Clock::now().time_since_epoch().count());
			g_bridge = bridge;
			g_worker = std::jthread([bridge](std::stop_token stop) { Worker(stop, bridge); });
			REX::DEBUG("[API] alignment file interface enabled at {}", bridge->directory.string());
			return true;
		} catch (const std::exception& error) {
			if (g_bridge) g_bridge->active = false;
			REX::ERROR("[API] cannot enable alignment tools: {}", error.what());
			return false;
		}
	}
}
