#include "API/OSFSceneAPI.h"

#include "API/NativeSceneEventRegistry.h"
#include "API/SceneAPIControl.h"
#include "Animation/GraphManager.h"
#include "Animation/Scene.h"
#include "Scene/SceneLauncher.h"
#include "Scene/SceneRuntime.h"
#include "Scene/SceneEventRelay.h"
#include "Util/ClipPath.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace OSF::API
{
	namespace
	{
		constexpr std::size_t kStartOptionsV1Size = offsetof(OSFStartOptions, inPlaceMode);

		bool ValidStartOptions(const OSFStartOptions& a_opts)
		{
			if (a_opts.size < kStartOptionsV1Size) {
				REX::WARN("[API] rejected OSFStartOptions with size {} (minimum {})", a_opts.size, kStartOptionsV1Size);
				return false;
			}
			return true;
		}

		Scene::SceneRuntime::StartOverrides MakeOverrides(const OSFStartOptions& a_opts)
		{
			// camera may lack a NUL terminator.
			const std::size_t len = ::strnlen(a_opts.camera, sizeof(a_opts.camera));
			// inPlaceMode (minor 2) only when the consumer's stamped size covers it.
			std::int32_t inPlaceMode = -1;
			if (a_opts.size >= offsetof(OSFStartOptions, inPlaceMode) + sizeof(a_opts.inPlaceMode)) {
				inPlaceMode = a_opts.inPlaceMode;
			}
			return Scene::MakeOverrides(a_opts.stripMode, a_opts.lockPlayerMode, a_opts.playerControlMode,
				a_opts.fadeMode, inPlaceMode, std::string_view(a_opts.camera, len), a_opts.loopScale);
		}

		Scene::SceneRuntime::AnchorOverride MakeAnchor(const OSFStartOptions& a_opts)
		{
			Scene::SceneRuntime::AnchorOverride anchor{};
			if (a_opts.anchorRef) {
				anchor.set = true;
				anchor.pos = a_opts.anchorRef->data.location;
				anchor.heading = a_opts.anchorRef->data.angle.z;
			} else if (a_opts.hasAnchor) {
				anchor.set = true;
				anchor.pos = RE::NiPoint3{ a_opts.anchorX, a_opts.anchorY, a_opts.anchorZ };
				anchor.heading = a_opts.anchorHeadingRad;
			}
			return anchor;
		}

		bool CollectActors(RE::Actor* const* a_actors, std::uint32_t a_count, std::vector<RE::Actor*>& a_out)
		{
			a_out.clear();
			if (!a_actors || a_count == 0) {
				return false;
			}
			a_out.reserve(a_count);
			for (std::uint32_t i = 0; i < a_count; i++) {
				if (!a_actors[i]) {
					a_out.clear();
					return false;
				}
				a_out.push_back(a_actors[i]);
			}
			return true;
		}

		Scene::DefLaunchOptions MakeDefLaunchOptions(const OSFStartOptions& a_opts)
		{
			Scene::DefLaunchOptions launch;
			launch.overrides = MakeOverrides(a_opts);
			launch.anchorRef = a_opts.anchorRef;
			if (!a_opts.anchorRef) {
				launch.worldAnchor = MakeAnchor(a_opts);
			}
			launch.stage = a_opts.startStage;
			// Preserve native ABI behavior; Papyrus rejects out-of-range linear stages.
			launch.invalidStage = Scene::InvalidStagePolicy::kUseEntry;
			launch.speed = a_opts.speed;
			launch.emitHud = false;
			return launch;
		}

		// OSFStartOptions ABI guard: append fields only at the end and bump the MINOR.
		static_assert(std::is_standard_layout_v<OSFStartOptions>, "OSFStartOptions must be standard-layout (POD ABI)");
		static_assert(sizeof(OSFStartOptions) == 136, "OSFStartOptions layout changed — update the size + version, ship a new header");
		static_assert(offsetof(OSFStartOptions, size) == 0, "OSFStartOptions.size must be first (cbSize prefix)");
		static_assert(offsetof(OSFStartOptions, camera) == 20, "OSFStartOptions.camera offset drift");
		static_assert(offsetof(OSFStartOptions, startStage) == 96, "OSFStartOptions.startStage offset drift");
		static_assert(offsetof(OSFStartOptions, anchorHeadingRad) == 116, "OSFStartOptions.anchorHeadingRad offset drift");
		static_assert(offsetof(OSFStartOptions, anchorRef) == 120, "OSFStartOptions.anchorRef offset drift");
		static_assert(offsetof(OSFStartOptions, inPlaceMode) == 128, "OSFStartOptions.inPlaceMode offset drift (MINOR 2 append)");
		static_assert(sizeof(OSFStartOptions::camera) == 64, "OSFStartOptions.camera buffer size changed");

		static_assert(std::is_standard_layout_v<OSFSceneEvent>, "OSFSceneEvent must be standard-layout (POD ABI)");
		static_assert(sizeof(OSFSceneEvent) == 88, "OSFSceneEvent layout changed - bump the API version");
		static_assert(offsetof(OSFSceneEvent, size) == 0, "OSFSceneEvent.size must be first");
		static_assert(offsetof(OSFSceneEvent, sceneHandle) == 4, "OSFSceneEvent.sceneHandle offset drift");
		static_assert(offsetof(OSFSceneEvent, eventType) == 8, "OSFSceneEvent.eventType offset drift");
		static_assert(offsetof(OSFSceneEvent, node) == 16, "OSFSceneEvent.node offset drift");
		static_assert(offsetof(OSFSceneEvent, actor) == 48, "OSFSceneEvent.actor offset drift");
		static_assert(offsetof(OSFSceneEvent, loopIndex) == 64, "OSFSceneEvent.loopIndex offset drift");
		static_assert(offsetof(OSFSceneEvent, anchor) == 72, "OSFSceneEvent.anchor offset drift");
		static_assert(offsetof(OSFSceneEvent, result) == 80, "OSFSceneEvent.result offset drift");
	}

	class SceneAPIImpl final : public IOSFSceneAPI
	{
	public:
		static SceneAPIImpl& GetSingleton()
		{
			static SceneAPIImpl singleton;
			return singleton;
		}

		void          MarkReady() { _ready.store(true, std::memory_order_release); }
		IOSFSceneAPI* IfReady() { return _ready.load(std::memory_order_acquire) ? this : nullptr; }

		std::uint32_t GetInterfaceVersion() override { return kOSFSceneAPIVersion; }

		void GetPluginVersion(std::uint32_t& a_major, std::uint32_t& a_minor, std::uint32_t& a_patch) override
		{
			const auto v = SFSE::GetPluginVersion();
			a_major = v.major();
			a_minor = v.minor();
			a_patch = v.patch();
		}

		bool IsReady() override { return Animation::GraphManager::GetSingleton().HooksInstalled(); }

		std::int32_t StartScene(RE::Actor* const* a_actors, std::uint32_t a_count, const char* a_sceneId,
			const OSFStartOptions& a_opts) override
		{
			std::vector<RE::Actor*> actors;
			if (!a_sceneId || !ValidStartOptions(a_opts) || !CollectActors(a_actors, a_count, actors)) {
				return 0;
			}
			return Scene::LaunchDef(a_sceneId, actors, MakeDefLaunchOptions(a_opts), "[API] StartScene");
		}

		std::int32_t StartSceneRoles(RE::Actor* const* a_actors, std::uint32_t a_count, const char* a_sceneId,
			const char* const* a_roles, std::uint32_t a_roleCount, const OSFStartOptions& a_opts) override
		{
			std::vector<RE::Actor*> actors;
			if (!a_sceneId || !ValidStartOptions(a_opts) || !CollectActors(a_actors, a_count, actors) ||
				!a_roles || a_roleCount != a_count) {
				return 0;
			}
			std::vector<std::string> roles;
			roles.reserve(a_roleCount);
			for (std::uint32_t i = 0; i < a_roleCount; i++) {
				roles.emplace_back(a_roles[i] ? a_roles[i] : "");
			}
			return Scene::LaunchDef(a_sceneId, actors, MakeDefLaunchOptions(a_opts), "[API] StartSceneRoles", &roles);
		}

		std::int32_t StartSceneFiles(RE::Actor* const* a_actors, const char* const* a_files, std::uint32_t a_count,
			const OSFStartOptions& a_opts) override
		{
			std::vector<RE::Actor*> actors;
			if (!a_files || !ValidStartOptions(a_opts) || !CollectActors(a_actors, a_count, actors)) {
				return 0;
			}
			Animation::PlaybackPlan          plan;
			Animation::PlaybackPlan::Segment stage;
			stage.files.reserve(a_count);
			stage.animIds.reserve(a_count);
			for (std::uint32_t i = 0; i < a_count; i++) {
				auto [file, animId] = Util::SplitRuntimeClipSpec(a_files[i] ? a_files[i] : "");
				stage.files.push_back(std::move(file));
				stage.animIds.push_back(std::move(animId));
			}
			stage.loops = 0;  // one synthetic holding stage
			plan.stages.push_back(std::move(stage));
			plan.speed = a_opts.speed;
			plan.blendIn = a_opts.blendIn;
			// startStage doesn't apply to a one-stage hold.
			return Scene::SceneRuntime::GetSingleton().StartFromPlan(
				actors, std::move(plan), 0, MakeAnchor(a_opts), MakeOverrides(a_opts));
		}

		bool StopScene(std::int32_t a_handle) override
		{
			return Scene::SceneRuntime::GetSingleton().Stop(a_handle);
		}

		bool StopSceneForActor(RE::Actor* a_actor) override
		{
			if (!a_actor) {
				return false;
			}
			if (Scene::SceneRuntime::GetSingleton().StopForActor(a_actor)) {
				return true;
			}
			return Animation::GraphManager::GetSingleton().StopScene(a_actor);
		}

		bool SetStage(std::int32_t a_handle, std::int32_t a_stage) override
		{
			return Scene::SceneRuntime::GetSingleton().SetStage(a_handle, a_stage);
		}

		bool Advance(std::int32_t a_handle) override
		{
			return Scene::SceneRuntime::GetSingleton().Advance(a_handle);
		}

		bool Navigate(std::int32_t a_handle, const char* a_edgeId) override
		{
			return Scene::SceneRuntime::GetSingleton().Navigate(a_handle, a_edgeId ? a_edgeId : "");
		}

		bool Play(RE::Actor* a_actor, const char* a_file, const char* a_animId) override
		{
			if (!a_actor || !a_file) {
				return false;
			}
			return Animation::GraphManager::GetSingleton().PlayAnimation(a_actor, a_file, a_animId ? a_animId : "");
		}

		bool Stop(RE::Actor* a_actor) override
		{
			if (!a_actor) {
				return false;
			}
			return Animation::GraphManager::GetSingleton().StopAnimation(a_actor);
		}

		bool SetSpeed(RE::Actor* a_actor, float a_speed) override
		{
			if (!a_actor) {
				return false;
			}
			return Animation::GraphManager::GetSingleton().SetSpeed(a_actor, a_speed);
		}

		float GetSpeed(RE::Actor* a_actor) override
		{
			if (!a_actor) {
				return 0.0f;
			}
			return Animation::GraphManager::GetSingleton().GetSpeed(a_actor);
		}

		bool IsPlaying(RE::Actor* a_actor) override
		{
			if (!a_actor) {
				return false;
			}
			return Animation::GraphManager::GetSingleton().IsPlaying(a_actor);
		}

		std::int32_t GetSceneForActor(RE::Actor* a_actor) override
		{
			if (!a_actor) {
				return 0;
			}
			return Scene::SceneRuntime::GetSingleton().GetSceneForActor(a_actor);
		}

		std::int32_t GetSceneStage(std::int32_t a_handle) override
		{
			return Scene::SceneRuntime::GetSingleton().GetStage(a_handle);
		}

		std::uint32_t GetSceneParticipants(std::int32_t a_handle, RE::Actor** a_out, std::uint32_t a_cap) override
		{
			const auto          parts = Scene::SceneRuntime::GetSingleton().GetParticipants(a_handle);
			const std::uint32_t total = static_cast<std::uint32_t>(parts.size());
			if (a_out && a_cap) {
				const std::uint32_t n = std::min(total, a_cap);
				for (std::uint32_t i = 0; i < n; i++) {
					a_out[i] = parts[i];
				}
			}
			return total;
		}

		std::uint64_t RegisterSceneEventCallback(
			OSFSceneEventCallback a_callback, void* a_context,
			std::int32_t a_sceneFilter,
			std::int32_t a_eventMask) override
		{
			const auto token = _nativeSceneEvents.Register(
				a_callback, a_context, a_sceneFilter, a_eventMask);
			if (token != 0) {
				REX::DEBUG(
					"[API] registered native scene-event token {:#018x} "
					"(mask {:#x}, scene {})",
					token, a_eventMask, a_sceneFilter);
			}
			return token;
		}

		bool UnregisterSceneEventCallback(std::uint64_t a_token) override
		{
			const bool removed = _nativeSceneEvents.Unregister(a_token);
			if (removed) {
				REX::DEBUG(
					"[API] unregistered native scene-event token {:#018x}",
					a_token);
			}
			return removed;
		}

		void DispatchNativeSceneEvent(const OSFSceneEvent& a_event)
		{
			const auto failures = _nativeSceneEvents.Dispatch(a_event);
			if (failures != 0) {
				REX::ERROR(
					"[API] {} native scene-event callback(s) threw; "
					"exceptions were isolated",
					failures);
			}
		}

	private:
		SceneAPIImpl() = default;

		std::atomic<bool>        _ready{ false };
		NativeSceneEventRegistry _nativeSceneEvents;
	};

	void MarkReady()
	{
		SceneAPIImpl::GetSingleton().MarkReady();
		REX::INFO("[API] native scene API ready (ABI {:#x})", kOSFSceneAPIVersion);
	}

	void DispatchNativeSceneEvent(const Scene::SceneEvent& a_event)
	{
		const OSFSceneEvent event{
			.size = sizeof(OSFSceneEvent),
			.sceneHandle = a_event.scene,
			.eventType = a_event.event,
			.node = a_event.node.c_str(),
			.edge = a_event.edge.c_str(),
			.cue = a_event.cue.c_str(),
			.actionType = a_event.actionType.c_str(),
			.actor = a_event.actor,
			.role = a_event.role.c_str(),
			.loopIndex = a_event.loopIndex,
			.time = a_event.time,
			.anchor = a_event.trackPosition.c_str(),  // frozen ABI field name
			.result = a_event.result
		};
		SceneAPIImpl::GetSingleton().DispatchNativeSceneEvent(event);
	}
}

// Returns nullptr on ABI mismatch or before the API is marked ready.
extern "C" __declspec(dllexport) OSF::API::IOSFSceneAPI* OSF_RequestSceneAPI(std::uint32_t a_abiVersion)
{
	if ((a_abiVersion >> 16) != OSF::API::kOSFSceneAPIMajor ||
		(a_abiVersion & 0xFFFFu) > OSF::API::kOSFSceneAPIMinor) {
		return nullptr;
	}
	return OSF::API::SceneAPIImpl::GetSingleton().IfReady();
}
