#include "API/EngineServices.h"
#include "API/ActorStateAPI.h"
#include "API/CatalogBuilder.h"
#include "API/EngineServiceState.h"
#include "API/OSFEngineAPI.h"
#include "API/PreparationAPI.h"
#include "API/SceneQuery.h"
#include "ActorState/Service.h"
#include "Animation/GraphManager.h"
#include "Audio/SoundService.h"
#include "Camera/CameraService.h"
#include "Input/InputService.h"
#include "Matchmaking/Matchmaker.h"
#include "Packs/PackReload.h"
#include "Registry/ContentRegistry.h"
#include "Scene/AnchorResolve.h"
#include "Scene/SceneRuntime.h"
#include "Serialization/ClipDurationProbe.h"
#include "Serialization/ClipDurations.h"
#include "Util/MonotonicClock.h"
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
		bool ReadClipDurations(TextCallback callback, void* context)
		{
			try { return Send(Serialization::ClipDurations::Snapshot(), callback, context); }
			catch (...) { return false; }
		}
		bool ProbeClipDuration(const char* file, const char* animId, const ClipProbeResult* previous, ClipProbeResult* result)
		{
			if (!file || !*file || !result || result->size < sizeof(ClipProbeResult) ||
				(previous && previous->size < sizeof(ClipProbeResult))) return false;
			try { *result = Serialization::ClipDurationProbe::Probe(file, animId ? animId : "", previous); return true; }
			catch (...) { return false; }
		}
		bool RegisterClipSceneProvider(const char* owner, ClipSceneProvider provider, void* context)
		{
			if (!owner || !provider) return false;
			try {
				const bool registered = Registry::ContentRegistry::GetSingleton().RegisterClipProvider(owner,
					[provider, context](const Registry::ContentRegistrySnapshot& source) {
						struct Output { std::uint32_t calls{}; std::optional<std::vector<ClipScene>> scenes; } output;
						const auto text = json(CatalogBuilder::Build(source, {})).dump(-1, ' ', false, json::error_handler_t::replace);
						provider(text.c_str(), [](const char* text, void* value) noexcept {
							auto& out = *static_cast<Output*>(value);
							++out.calls;
							try {
								if (text && std::strlen(text) <= 16 * 1024 * 1024) out.scenes = json::parse(text).get<std::vector<ClipScene>>();
							} catch (...) { out.scenes.reset(); }
						}, &output, context);
						if (output.calls != 1 || !output.scenes) throw std::runtime_error("invalid provider response");
						return std::move(*output.scenes);
					});
				if (registered) CatalogChanged();
				return registered;
			} catch (...) { return false; }
		}
		const OSFEngineAPI g_api{ sizeof(OSFEngineAPI), kOSFEngineAPIVersion, [](FrameCallback callback, void* context) -> bool { return g_state.Post(callback, context); },
			[](EngineCallback callback, void* context) -> std::uint64_t {
				if (!callback)
					return 0;
				try {
					return g_state.Subscribe([=](std::uint32_t event) { callback(static_cast<EngineEvent>(event), context); });
				} catch (...) {
					return 0;
				}
			},
			[](std::uint64_t token) { return g_state.Unsubscribe(token); }, &ReadCatalog, &ReadClipDurations, &ProbeClipDuration, &Content::ReloadAll, &MatchCast, &MatchAnchor, &ReadActiveScenes,
			[](std::int32_t handle) {
				auto& rt = Scene::SceneRuntime::GetSingleton();
				if (rt.GetParticipants(handle).empty())
					return false;
				rt.SetSingleStage(handle);
				return true;
			},
			[](std::int32_t handle, float speed) {
				if (!std::isfinite(speed) || speed < 0)
					return false;
				return Scene::SceneRuntime::GetSingleton().SetSpeed(handle, speed);
			},
			[](std::int32_t handle) { return Scene::SceneRuntime::GetSingleton().GetSpeed(handle); },
			[](RE::Actor* actor, TextCallback callback, void* context) {
				try {
					return actor && Send(json{ { "species", Util::ActorSpecies(actor) }, { "gender", Matchmaking::ActorGenderTag(actor) } }, callback, context);
				} catch (...) {
					return false;
				}
			},
			[](RE::TESObjectREFR* ref, float* position, float* heading) {
				if (!position || !heading)
					return false;
				const auto anchor=Scene::MakeAnchorAt(ref, std::nullopt);
				if (!anchor.set)
					return false;
				position[0]=anchor.pos.x;
				position[1] = anchor.pos.y;
				position[2] = anchor.pos.z;
				*heading = anchor.heading;
				return true;
			},
			[](RE::TESObjectREFR* ref, const float* position, float* snapped) {
				if (!ref || !position || !snapped || !std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2]))
					return false;
				const auto result = Scene::SnapWorldPosition(ref, {position[0], position[1], position[2]});
				if (!result)
					return false;
				snapped[0]=result->x;
				snapped[1] = result->y;
				snapped[2] = result->z;
				return true;
			},
			[](const std::uint32_t* ids, std::uint32_t count) {
				if (count > 32 || (count && !ids))
					return false;
				return Camera::CameraService::GetSingleton().AcquireInspectionOrbit(count ? std::vector<std::uint32_t>(ids,ids+count) : std::vector<std::uint32_t>{});
			},
			[] { Camera::CameraService::GetSingleton().ReleaseInspectionOrbit(); },
			[](float* position, float* forward) {
				if (!position || !forward)
					return false;
				float p[3], f[3];
				if (!Camera::CameraService::GetSingleton().SceneOrbitPose(p,f))
					return false;
				std::copy_n(p,3,position);
				std::copy_n(f, 3, forward);
				return true;
			},
			[] { Camera::CameraService::GetSingleton().KickToThirdPerson(); }, [](std::uint32_t flags) { Input::InputService::GetSingleton().SetInputRouting(flags); },
			[](float dx, float dy, float wheel) {
				if (std::isfinite(dx) && std::isfinite(dy) && std::isfinite(wheel))
					Input::InputService::GetSingleton().InjectOrbitDelta(dx, dy, wheel);
			},
			&RegisterClipSceneProvider,
			[](const char* owner) {
				try {
					if (!owner || !Registry::ContentRegistry::GetSingleton().UnregisterClipProvider(owner))
						return false;
					CatalogChanged();
					return true;
				} catch (...) {
					return false;
				}
			},
			[](std::int32_t handle, const char* role, const float* d) { return role && d && Scene::SceneRuntime::GetSingleton().SetAlignment(handle, role, { d[0], d[1], d[2], d[3] * Util::kDegToRadF }); },
			[](std::int32_t handle, const char* role, float* d) {
				if (!role || !d)
					return false;
				const auto value = Scene::SceneRuntime::GetSingleton().GetAlignment(handle, role);
				if (!value)
					return false;
				d[0] = value->x;
				d[1] = value->y;
				d[2] = value->z;
				d[3] = value->heading / Util::kDegToRadF;
				return true;
			},
			[](std::int32_t handle) { return Scene::SceneRuntime::GetSingleton().SaveAlignment(handle); }, [](std::int32_t handle, const char* role) { return role && Scene::SceneRuntime::GetSingleton().SetAlignment(handle, role, {}); },
			[](std::int32_t handle, const char* id, const OSFStartOptions* opts) {
				if (!id || (opts && opts->size < offsetof(OSFStartOptions, inPlaceMode)))
					return false;
				Scene::SceneRuntime::ReplaceOptions options;
				if (opts) {
					options.anchor = opts->anchorRef;
					options.stage = opts->startStage;
				}
				return Scene::SceneRuntime::GetSingleton().Replace(handle, id, {}, options);
			},
			[](std::int32_t handle, const char* id, const char* const* roles, std::uint32_t count) {
				if (!id || (!roles && count) || count > 65535)
					return false;
				std::vector<std::string> names;
				for (std::uint32_t i = 0; i < count; ++i) {
					if (!roles[i])
						return false;
					names.emplace_back(roles[i]);
				}
				return Scene::SceneRuntime::GetSingleton().Replace(handle, id, names, {});
			},
			[](std::int32_t handle, const OSFSceneQuery* query) {
				if (!query)
					return false;
				auto& rt = Scene::SceneRuntime::GetSingleton();
				std::string error;
				const auto in = ReadSceneQuery(*query, rt.GetParticipants(handle).size(), error);
				if (!in) {
					REX::WARN("[API] ReplaceSceneByQuery {:#010x}: {}", handle, error);
					return false;
				}
				return rt.ReplaceByQuery(handle, in->tags, in->roles, in->anchor, in->mode);
			},
			[](std::int32_t handle, bool enabled) { return Scene::SceneRuntime::GetSingleton().SetAutoAdvance(handle, enabled); },
			[](std::int32_t handle, TextCallback callback, void* context) {
				try {
					auto& rt = Scene::SceneRuntime::GetSingleton();
					const auto node = rt.GetNode(handle);
					if (node.empty())
						return false;
					json edges = json::array();
					for (std::int32_t i = 0; i < rt.EdgeCount(handle); ++i)
						edges.push_back({{"id", rt.EdgeId(handle, i)}, {"label", rt.EdgeLabel(handle, i)}});
					return Send(json{{"sceneId", rt.GetSceneId(handle)}, {"node", node},
						{"automatic", rt.GetAutoAdvance(handle)}, {"allowManualHold", rt.AllowsManualHold(handle)},
						{"speed", rt.GetSpeed(handle)}, {"edges", std::move(edges)}}, callback, context);
				} catch (...) {
					return false;
				}
			},
			[]() -> std::uint64_t {
				try {
					return Audio::SoundService::GetSingleton().CreateChannel();
				} catch (...) {
					return 0;
				}
			},
			[](std::uint64_t channel, const char* spec) {
				try {
					return spec && Audio::SoundService::GetSingleton().PlayChannel(channel, spec);
				} catch (...) {
					return false;
				}
			},
			[](std::uint64_t channel) {
				try {
					return Audio::SoundService::GetSingleton().CloseChannel(channel);
				} catch (...) {
					return false;
				}
			},
			[](RE::Actor* actor, std::uint64_t channel) {
				try {
					return actor && Audio::SoundService::GetSingleton().BindSlot((1ull << 62) | actor->GetFormID(), channel);
				} catch (...) {
					return false;
				}
			},
			[](const AudioChannelOptions* options) -> std::uint64_t {
				if (!options || options->size < sizeof(AudioChannelOptions) || options->latestPending > 1)
					return 0;
				try {
					return Audio::SoundService::GetSingleton().CreateChannel(options->latestPending != 0, options->maxAgeMs);
				} catch (...) {
					return 0;
				}
			},
			&Util::MonotonicNs,
			[](std::uint64_t channel, const char* spec, std::uint64_t detectedNs) {
				try {
					return spec && detectedNs && Audio::SoundService::GetSingleton().PlayChannel(channel, spec, detectedNs);
				} catch (...) {
					return false;
				}
			},
			&ActorStateAPI::CreateScope, &ActorStateAPI::AcquireEquipment, &ActorStateAPI::ReleaseClaim, &ActorStateAPI::ReleaseGroup, &ActorStateAPI::CloseScope, &ActorStateAPI::Reconcile, &PreparationAPI::Register, &PreparationAPI::Unregister, &PreparationAPI::Equipment,
			&PreparationAPI::Resource, &PreparationAPI::Failure, &PreparationAPI::Read, &ActorStateAPI::ReadEquipment,
			&ActorStateAPI::ReadRevision, &ActorStateAPI::ReadChanges, &PreparationAPI::Guard,
			[](std::int32_t scene, const char* id, const char* const* roles, std::uint32_t count) {
				if (!Util::NativeMainThreadQueue::IsDraining() || !id || (!roles && count) || count > kOSFSceneMatchMaxActors) return false;
				std::vector<std::string> names;
				for (std::uint32_t i=0;i<count;++i) { if (!roles[i]) return false; names.emplace_back(roles[i]); }
				return Scene::SceneRuntime::GetSingleton().Replace(scene,id,names,{.covered=true});
			} };
	}
	void WorldReset() { ActorState::Get().Reset(); g_state.ResetWorld(); }
	void ContentReloaded() { g_state.Notify(static_cast<std::uint32_t>(EngineEvent::ContentReloaded)); }
	void CatalogChanged() { g_state.Notify(static_cast<std::uint32_t>(EngineEvent::CatalogChanged)); }
	void ScenesChanged() { g_state.Notify(static_cast<std::uint32_t>(EngineEvent::ScenesChanged)); }
}

extern "C" __declspec(dllexport) const OSF::API::OSFEngineAPI* OSF_RequestEngineAPI(std::uint32_t version)
{
	if (!OSF::API::EngineRequestSupported(version, OSF::API::kOSFEngineAPIVersion) || !OSF::API::RequestSceneAPI()) return nullptr;
	static const auto legacy = OSF::API::LegacyEngineAPI(OSF::API::EngineServices::g_api);
	if (version == 0x00030000) return &legacy;
	return &OSF::API::EngineServices::g_api;
}
