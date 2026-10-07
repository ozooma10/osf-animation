#include "API/OSFEngineAPI.h"
#include "API/CatalogBuilder.h"
#include "API/EngineServices.h"
#include "API/EngineServiceState.h"
#include "Animation/GraphManager.h"
#include "Camera/CameraService.h"
#include "Input/InputService.h"
#include "Matchmaking/Matchmaker.h"
#include "Packs/PackReload.h"
#include "Registry/ContentRegistry.h"
#include "Scene/AnchorResolve.h"
#include "Scene/SceneRuntime.h"
#include "Serialization/ClipDurationCatalog.h"
#include "Util/NativeMainThreadQueue.h"
#include "Util/Species.h"

namespace OSF::API::EngineServices
{
	namespace
	{
		EngineServiceState g_state;
		using json = nlohmann::json;
		bool Send(const json& value, TextCallback callback, void* context)
		{
			if (!callback) return false;
			const auto text = value.dump(-1, ' ', false, json::error_handler_t::replace);
			callback(text.c_str(), context);
			return true;
		}
		bool ReadCatalog(TextCallback callback, void* context)
		{
			try {
				const auto registry = Registry::ContentRegistry::GetSingleton().ReadSnapshot();
				std::error_code ec;
				auto root = std::filesystem::current_path(ec);
				if (ec) root.clear();
				else root /= "Data/OSF";
				return Send(CatalogBuilder::Build(*registry, root), callback, context);
			} catch (...) { REX::ERROR("[API] Could not read catalog metadata"); return false; }
		}
		bool MatchCast(RE::Actor* const* actors, std::uint32_t count, TextCallback callback, void* context)
		{
			if (!actors || count == 0 || count > 32) return false;
			for (std::uint32_t i=0; i<count; ++i) if (!actors[i]) return false;
			try {
				json ids=json::array();
				Registry::ContentRegistry::GetSingleton().ForEachDef([&](const Registry::SceneDef& d) {
					if (!d.clipsAvailable || CatalogBuilder::ActorCount(d) != count) return;
					for (std::size_t i=0; i<d.roles.size(); ++i) if (!Matchmaking::RoleAccepts(d.roles[i], actors[i])) return;
					ids.push_back(d.id);
				});
				return Send(ids, callback, context);
			} catch (...) { return false; }
		}
		bool MatchAnchor(RE::TESObjectREFR* ref, TextCallback callback, void* context)
		{
			if (!ref) return false;
			try {
				json ids=json::array();
				Matchmaking::AnchorMatchCache cache(ref);
				Registry::ContentRegistry::GetSingleton().ForEachDef([&](const Registry::SceneDef& d) {
					if (d.clipsAvailable && d.RequiresAnchor() && cache.Accepts(d)) ids.push_back(d.id);
				});
				return Send(ids, callback, context);
			} catch (...) { return false; }
		}
		bool ReadActiveScenes(TextCallback callback, void* context)
		{
			try {
				json scenes=json::array();
				auto& rt=Scene::SceneRuntime::GetSingleton();
				for (const auto& s : rt.ListScenes()) {
					json participants=json::array();
					for (auto* actor : s.participants) if (actor) participants.push_back(actor->GetFormID());
					json value={{"handle",s.handle},{"id",s.id},{"stage",rt.GetStage(s.handle)},{"participants",participants}};
					if (!s.participants.empty()) {
						if (const auto p=Animation::GraphManager::GetSingleton().GetScenePlayback(s.participants.front())) {
							if (s.id.empty()) value["stage"]=p->stage;
							value["time"]=p->time; value["duration"]=p->duration; value["speed"]=p->speed;
						}
					}
					scenes.push_back(std::move(value));
				}
				return Send(scenes,callback,context);
			} catch (...) { return false; }
		}
		void ScanDurations() { Serialization::ClipDurationCatalog::ScanSceneClipsAsync(&CatalogChanged); }
		const OSFEngineAPI g_api{
			sizeof(OSFEngineAPI), kOSFEngineAPIVersion,
			[](FrameCallback callback, void* context) -> bool {
				return g_state.Post(callback, context);
			},
			[](EngineCallback callback, void* context) -> std::uint64_t {
				if (!callback) return 0;
				try { return g_state.Subscribe([=](std::uint32_t event) { callback(static_cast<EngineEvent>(event),context); }); } catch (...) { return 0; }
			},
			[](std::uint64_t token) { return g_state.Unsubscribe(token); },
			&ReadCatalog, &ScanDurations, &Content::ReloadAll, &MatchCast, &MatchAnchor, &ReadActiveScenes,
			[](std::int32_t handle) { auto& rt = Scene::SceneRuntime::GetSingleton(); if (rt.GetParticipants(handle).empty()) return false; rt.SetSingleStage(handle); return true; },
			[](std::int32_t handle, float speed) {
				if (!std::isfinite(speed) || speed < 0) return false;
				const auto actors=Scene::SceneRuntime::GetSingleton().GetParticipants(handle);
				return !actors.empty() && Animation::GraphManager::GetSingleton().SetSpeed(actors.front(), speed);
			},
			[](std::int32_t handle) {
				const auto actors=Scene::SceneRuntime::GetSingleton().GetParticipants(handle);
				return actors.empty() ? -1.0f : Animation::GraphManager::GetSingleton().GetSpeed(actors.front());
			},
			[](RE::Actor* actor, TextCallback callback, void* context) {
				try { return actor && Send(json{{"species",Util::ActorSpecies(actor)},{"gender",Matchmaking::ActorGenderTag(actor)}},callback,context); } catch (...) { return false; }
			},
			[](RE::Actor* actor, float distance, OSFStartOptions* options) {
				if (!options || options->size < sizeof(OSFStartOptions) || !std::isfinite(distance)) return false;
				const auto anchor=Scene::MakeAnchorInFrontOfView(actor,distance);
				if (!anchor.set) return false;
				options->anchorRef=nullptr; options->hasAnchor=true;
				options->anchorX=anchor.pos.x; options->anchorY=anchor.pos.y; options->anchorZ=anchor.pos.z; options->anchorHeadingRad=anchor.heading;
				return true;
			},
			[](const std::uint32_t* ids, std::uint32_t count) {
				if (count > 32 || (count && !ids)) return false;
				return Camera::CameraService::GetSingleton().AcquireInspectionOrbit(count ? std::vector<std::uint32_t>(ids,ids+count) : std::vector<std::uint32_t>{});
			},
			[] { Camera::CameraService::GetSingleton().ReleaseInspectionOrbit(); },
			[](float* position, float* forward) {
				if (!position || !forward) return false;
				float p[3], f[3];
				if (!Camera::CameraService::GetSingleton().SceneOrbitPose(p,f)) return false;
				std::copy_n(p,3,position); std::copy_n(f,3,forward); return true;
			},
			[] { Camera::CameraService::GetSingleton().KickToThirdPerson(); },
			[](bool visible) { Input::InputService::GetSingleton().SetUiCursorVisible(visible); },
			[](float dx,float dy,float wheel) { if (std::isfinite(dx) && std::isfinite(dy) && std::isfinite(wheel)) Input::InputService::GetSingleton().InjectOrbitDelta(dx,dy,wheel); }
		};
	}
	void WorldReset() { g_state.ResetWorld(); }
	void ContentReloaded() { g_state.Notify(static_cast<std::uint32_t>(EngineEvent::ContentReloaded)); }
	void CatalogChanged() { g_state.Notify(static_cast<std::uint32_t>(EngineEvent::CatalogChanged)); }
	void ScenesChanged() { g_state.Notify(static_cast<std::uint32_t>(EngineEvent::ScenesChanged)); }
}

extern "C" __declspec(dllexport) const OSF::API::OSFEngineAPI* OSF_RequestEngineAPI(std::uint32_t version)
{
	if (version != OSF::API::kOSFEngineAPIVersion || !OSF::API::RequestSceneAPI()) return nullptr;
	return &OSF::API::EngineServices::g_api;
}
