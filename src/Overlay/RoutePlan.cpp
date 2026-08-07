#include "Overlay/RoutePlan.h"

#include "Util/StringUtil.h"

#include <algorithm>
#include <deque>
#include <unordered_map>

namespace OSF::Overlay
{
	using Util::ToLower;

	std::vector<const Registry::RouteTransition*> ShortestPath(
		const Registry::RouteDef& a_route, std::string_view a_from, std::string_view a_to)
	{
		const std::string from = ToLower(std::string(a_from));
		const std::string to = ToLower(std::string(a_to));
		if (from == to) return {};

		struct Previous
		{
			std::string station;
			const Registry::RouteTransition* transition = nullptr;
		};
		std::deque<std::string> queue{ from };
		std::unordered_map<std::string, Previous> previous;
		previous.emplace(from, Previous{});
		while (!queue.empty()) {
			const std::string current = std::move(queue.front());
			queue.pop_front();
			for (const auto& transition : a_route.transitions) {
				if (ToLower(transition.from) != current) continue;
				const auto next = ToLower(transition.to);
				if (previous.contains(next)) continue;
				previous.emplace(next, Previous{ current, &transition });
				if (next == to) {
					std::vector<const Registry::RouteTransition*> path;
					for (std::string cursor = to; cursor != from;) {
						const auto& step = previous.at(cursor);
						path.push_back(step.transition);
						cursor = step.station;
					}
					std::reverse(path.begin(), path.end());
					return path;
				}
				queue.push_back(next);
			}
		}
		return {};
	}

	RouteController::RouteController(const Registry::RouteDef& a_route, std::string a_initialStation,
		IRoutePlayback& a_playback) :
		_route(a_route), _playback(a_playback), _reached(std::move(a_initialStation)),
		_checkpoint(_reached), _desired(_reached)
	{}

	RequestResult RouteController::Start()
	{
		if (!_route.FindStation(_reached)) return Fail(RequestReason::kUnknownStation);
		return Reconcile();
	}

	RequestResult RouteController::RequestStation(std::string_view a_station, std::uint64_t a_token)
	{
		if (_phase == ControllerPhase::kFailed) return { RequestDisposition::kRejected, RequestReason::kPlaybackFailed };
		const auto* station = _route.FindStation(a_station);
		if (!station) return { RequestDisposition::kRejected, RequestReason::kUnknownStation };
		_requestToken = a_token;
		const bool same = ToLower(_desired) == ToLower(station->id);
		_desired = station->id;
		if (Blocker() != ControllerBlocker::kNone) {
			return { RequestDisposition::kPending,
				Blocker() == ControllerBlocker::kScene ? RequestReason::kSceneBlocked : RequestReason::kActorUnavailable };
		}
		if (_phase == ControllerPhase::kTransitioning) {
			if (!same && _active && !_commitAcknowledged &&
				_active->interruption == Registry::RouteInterruption::kCrossfadeBeforeCommit) {
				CountermandBeforeCommit();
				return Reconcile();
			}
			return { RequestDisposition::kPending, RequestReason::kNone };
		}
		return Reconcile();
	}

	bool RouteController::SetBlocker(ControllerBlocker a_blocker)
	{
		if (a_blocker == ControllerBlocker::kNone || _phase == ControllerPhase::kFailed) return false;
		if ((a_blocker == ControllerBlocker::kScene && _sceneBlocked) ||
			(a_blocker == ControllerBlocker::kActor3D && _actorBlocked)) return false;
		const bool wasBlocked = Blocker() != ControllerBlocker::kNone;
		if (a_blocker == ControllerBlocker::kScene) _sceneBlocked = true;
		else if (a_blocker == ControllerBlocker::kActor3D) _actorBlocked = true;
		if (wasBlocked) return true;
		if (_phase == ControllerPhase::kTransitioning) {
			_playback.Stop(false);
			_active = nullptr;
			_commitAcknowledged = false;
			_reached = _checkpoint;
			_phase = ControllerPhase::kAtStation;
			_stationApplied = false;
		} else {
			_playback.Stop(false);
			_stationApplied = false;
		}
		return true;
	}

	bool RouteController::ClearBlocker(ControllerBlocker a_blocker)
	{
		if (a_blocker == ControllerBlocker::kScene) {
			if (!_sceneBlocked) return false;
			_sceneBlocked = false;
		} else if (a_blocker == ControllerBlocker::kActor3D) {
			if (!_actorBlocked) return false;
			_actorBlocked = false;
		} else {
			return false;
		}
		if (Blocker() != ControllerBlocker::kNone) return true;
		(void)Reconcile();
		return true;
	}

	bool RouteController::OnCommit(std::uint32_t a_generation, bool a_acknowledged)
	{
		if (_phase != ControllerPhase::kTransitioning || !_active || a_generation != _transitionGeneration) return false;
		if (!a_acknowledged) {
			_playback.Stop(true);
			Fail(RequestReason::kHandoffRejected);
			return true;
		}
		_commitAcknowledged = true;
		_checkpoint = _active->to;
		return true;
	}

	bool RouteController::OnTransitionReached(std::uint32_t a_generation)
	{
		if (_phase != ControllerPhase::kTransitioning || !_active || a_generation != _transitionGeneration) return false;
		if (_active->commit && !_commitAcknowledged) {
			_playback.Stop(true);
			Fail(RequestReason::kHandoffRejected);
			return true;
		}
		const auto destination = _active->to;
		if (!_active->commit) _checkpoint = destination;
		_reached = destination;
		_active = nullptr;
		_commitAcknowledged = false;
		_phase = ControllerPhase::kAtStation;
		_stationApplied = true;  // transition playback already carries the destination hold (or vanilla zero station)
		(void)Reconcile();
		return true;
	}

	bool RouteController::OnEdgeReached(std::uint32_t a_generation)
	{
		return OnTransitionReached(a_generation);
	}

	void RouteController::End(bool a_fade)
	{
		_playback.Stop(a_fade);
		_active = nullptr;
	}

	RequestResult RouteController::Reconcile()
	{
		if (Blocker() != ControllerBlocker::kNone) {
			return { RequestDisposition::kPending,
				Blocker() == ControllerBlocker::kScene ? RequestReason::kSceneBlocked : RequestReason::kActorUnavailable };
		}
		if (_phase == ControllerPhase::kFailed) return { RequestDisposition::kRejected, RequestReason::kPlaybackFailed };
		if (_phase == ControllerPhase::kTransitioning) return { RequestDisposition::kPending, RequestReason::kNone };
		const auto* current = _route.FindStation(_reached);
		const auto* desired = _route.FindStation(_desired);
		if (!current || !desired) return Fail(RequestReason::kUnknownStation);
		if (ToLower(current->id) == ToLower(desired->id)) {
			_lastReason = RequestReason::kNone;
			if (_stationApplied) return { RequestDisposition::kAccepted, RequestReason::kNone };
			if (!_playback.PlayStation(*current)) return Fail(RequestReason::kPlaybackFailed);
			_stationApplied = true;
			return { RequestDisposition::kAccepted, RequestReason::kNone };
		}
		auto path = ShortestPath(_route, current->id, desired->id);
		if (path.empty()) {
			_lastReason = RequestReason::kNoPath;
			return { RequestDisposition::kRejected, RequestReason::kNoPath };
		}
		_active = path.front();
		const auto* destination = _route.FindStation(_active->to);
		if (!destination) return Fail(RequestReason::kUnknownStation);
		if (++_transitionGeneration == 0) ++_transitionGeneration;
		_commitAcknowledged = false;
		_phase = ControllerPhase::kTransitioning;
		_stationApplied = false;
		if (!_playback.PlayTransition(*_active, *destination, _transitionGeneration)) {
			return Fail(RequestReason::kPlaybackFailed);
		}
		_lastReason = RequestReason::kNone;
		return { RequestDisposition::kAccepted, RequestReason::kNone };
	}

	RequestResult RouteController::Fail(RequestReason a_reason)
	{
		_phase = ControllerPhase::kFailed;
		_lastReason = a_reason;
		_active = nullptr;
		return { RequestDisposition::kRejected, a_reason };
	}

	void RouteController::CountermandBeforeCommit()
	{
		_playback.Stop(true);
		_active = nullptr;
		_commitAcknowledged = false;
		_phase = ControllerPhase::kAtStation;
		_stationApplied = false;
	}
}
