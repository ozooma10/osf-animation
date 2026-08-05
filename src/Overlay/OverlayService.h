#pragma once

#include "API/OSFOverlayAPI.h"
#include "Overlay/OwnerRegistry.h"
#include "Overlay/RoutePlan.h"
#include "Props/PropService.h"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace OSF::Overlay
{
	class OverlayService : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
	{
	public:
		static OverlayService& GetSingleton();
		~OverlayService();

		void Register();
		std::uint64_t AcquireOwner(std::string_view a_pluginId, API::OSFOverlayCallback a_callback, void* a_context);
		bool ReleaseOwner(std::uint64_t a_owner);
		BeginResult BeginRoute(std::uint64_t a_owner, RE::Actor* a_actor,
			std::string_view a_routeId, std::string_view a_initialStation);
		RequestResult RequestStation(std::int32_t a_handle, std::string_view a_station, std::uint64_t a_token);
		bool EndRoute(std::int32_t a_handle, bool a_fade, bool a_notify = true);
		std::int32_t GetRouteForActor(RE::Actor* a_actor) const;
		bool QueryRoute(std::int32_t a_handle, API::OSFOverlayRouteState& a_out) const;

		// SceneRuntime calls these after its slot admission and on every rollback/end path.
		void SuspendForScene(std::int32_t a_sceneHandle, const std::vector<RE::Actor*>& a_participants);
		void ReconcileAfterScene(std::int32_t a_sceneHandle);
		void ClearWorld();

		RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent& a_event,
			RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override;

	private:
		OverlayService() = default;
		OverlayService(const OverlayService&) = delete;
		OverlayService& operator=(const OverlayService&) = delete;

		class PlaybackAdapter;
		struct ActiveProp;
		struct Slot;

		Slot* Resolve(std::int32_t a_handle);
		const Slot* Resolve(std::int32_t a_handle) const;
		std::int32_t MintHandle();
		RequestResult StartRoute(std::int32_t a_handle);
		void DropRoute(std::int32_t a_handle);
		bool EndRouteLocked(std::int32_t a_handle, bool a_fade, bool a_notify);
		bool DispatchOwner(Slot& a_slot, API::OSFOverlayEvent& a_event, bool a_commit);
		void OnTimedMarks(Animation::PlaybackId a_playbackId,
			const std::vector<RE::Actor*>& a_actors, const std::vector<Animation::FiredMark>& a_marks);
		bool OnAutoEnd(Animation::PlaybackId a_playbackId,
			const std::vector<RE::Actor*>& a_actors, Animation::SceneEndReason a_reason);
		void CleanupProps(Slot& a_slot, std::optional<Registry::RouteLifetime> a_lifetime = std::nullopt);
		void SuspendProps(Slot& a_slot);
		void RestoreProps(Slot& a_slot);
		void PlayRouteSound(Slot& a_slot, std::string_view a_spec);

		mutable std::mutex _lock;
		std::vector<std::unique_ptr<Slot>> _slots;
		std::unordered_map<RE::Actor*, std::int32_t> _byActor;
		OwnerRegistry _owners;
		std::uint16_t _nextGeneration = 1;
		Animation::PlaybackSinkId _playbackSinkId = 0;
		bool _registered = false;
	};
}
