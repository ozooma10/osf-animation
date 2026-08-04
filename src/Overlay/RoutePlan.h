#pragma once

#include "Registry/SceneRegistry.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace OSF::Overlay
{
	enum class RequestDisposition : std::uint8_t
	{
		kAccepted,
		kPending,
		kRejected
	};

	enum class RequestReason : std::uint8_t
	{
		kNone,
		kInvalidHandle,
		kUnknownStation,
		kNoPath,
		kBusy,
		kSceneBlocked,
		kActorUnavailable,
		kPlaybackFailed,
		kHandoffRejected,
		kOwnerInvalid,
		kRouteUnknown,
		kDispatchDeferred
	};

	struct RequestResult
	{
		RequestDisposition disposition = RequestDisposition::kRejected;
		RequestReason reason = RequestReason::kInvalidHandle;
	};

	struct BeginResult
	{
		std::int32_t handle = 0;
		RequestDisposition disposition = RequestDisposition::kRejected;
		RequestReason reason = RequestReason::kOwnerInvalid;
	};

	enum class ControllerPhase : std::uint8_t
	{
		kAtStation,
		kTransitioning,
		kFailed
	};

	enum class ControllerBlocker : std::uint8_t
	{
		kNone,
		kScene,
		kActor3D
	};

	// Authored-order BFS. The returned pointers are pinned by the caller's RouteRef.
	std::vector<const Registry::RouteTransition*> ShortestPath(
		const Registry::RouteDef& a_route, std::string_view a_from, std::string_view a_to);

	class IRoutePlayback
	{
	public:
		virtual ~IRoutePlayback() = default;
		virtual bool PlayStation(const Registry::RouteStation& a_station) = 0;
		virtual bool PlayTransition(const Registry::RouteTransition& a_transition,
			const Registry::RouteStation& a_destination, std::uint32_t a_generation) = 0;
		virtual void Stop(bool a_fade) = 0;
	};

	// Pure route state machine. Engine objects, props, callbacks, and handles live in OverlayService;
	// tests drive this class through the tiny playback interface.
	class RouteController
	{
	public:
		RouteController(const Registry::RouteDef& a_route, std::string a_initialStation,
			IRoutePlayback& a_playback);

		RequestResult Start();
		RequestResult RequestStation(std::string_view a_station, std::uint64_t a_token);
		bool SetBlocker(ControllerBlocker a_blocker);
		bool ClearBlocker(ControllerBlocker a_blocker);
		bool OnCommit(std::uint32_t a_generation, bool a_acknowledged);
		bool OnEdgeReached(std::uint32_t a_generation);
		void End(bool a_fade);

		[[nodiscard]] ControllerPhase Phase() const noexcept { return _phase; }
		[[nodiscard]] ControllerBlocker Blocker() const noexcept
		{
			return _actorBlocked ? ControllerBlocker::kActor3D :
				(_sceneBlocked ? ControllerBlocker::kScene : ControllerBlocker::kNone);
		}
		[[nodiscard]] const std::string& ReachedStation() const noexcept { return _reached; }
		[[nodiscard]] const std::string& CheckpointStation() const noexcept { return _checkpoint; }
		[[nodiscard]] const std::string& DesiredStation() const noexcept { return _desired; }
		[[nodiscard]] std::uint64_t RequestToken() const noexcept { return _requestToken; }
		[[nodiscard]] std::uint32_t TransitionGeneration() const noexcept { return _transitionGeneration; }
		[[nodiscard]] const Registry::RouteTransition* ActiveTransition() const noexcept { return _active; }
		[[nodiscard]] RequestReason LastReason() const noexcept { return _lastReason; }

	private:
		RequestResult Reconcile();
		RequestResult Fail(RequestReason a_reason);
		void CountermandBeforeCommit();

		const Registry::RouteDef* _route = nullptr;
		IRoutePlayback& _playback;
		ControllerPhase _phase = ControllerPhase::kAtStation;
		bool _sceneBlocked = false;
		bool _actorBlocked = false;
		std::string _reached;
		std::string _checkpoint;
		std::string _desired;
		std::uint64_t _requestToken = 0;
		std::uint32_t _transitionGeneration = 0;
		const Registry::RouteTransition* _active = nullptr;
		bool _commitAcknowledged = false;
		bool _stationApplied = false;
		RequestReason _lastReason = RequestReason::kNone;
	};
}
