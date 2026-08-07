#include "Scene/PlaybackPreviewService.h"

#include "Matchmaking/Matchmaker.h"
#include "Overlay/OverlayService.h"
#include "Overlay/RoutePlaybackPlan.h"
#include "Scene/InspectionPropTimeline.h"
#include "Scene/RouteInspectionTimeline.h"
#include "Util/StringUtil.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace OSF::Scene
{
	PlaybackPreviewService& PlaybackPreviewService::GetSingleton()
	{
		static PlaybackPreviewService singleton;
		return singleton;
	}

	std::int32_t PlaybackPreviewService::MintHandle()
	{
		while (_nextHandle == 0 || _previews.contains(_nextHandle)) {
			_nextHandle = _nextHandle == (std::numeric_limits<std::int32_t>::min)() ? -1 : _nextHandle - 1;
		}
		const auto handle = _nextHandle;
		_nextHandle = _nextHandle == (std::numeric_limits<std::int32_t>::min)() ? -1 : _nextHandle - 1;
		return handle;
	}

	std::optional<PreparedPreview> PlaybackPreviewService::Prepare(
		const PreviewRequest& a_request, std::string& a_error) const
	{
		const auto& definition = a_request.definition;
		if (!definition) {
			a_error = "The selected scene definition is no longer loaded";
			return std::nullopt;
		}

		std::string nodeId = definition->entry;
		if (a_request.startStage < 0) {
			a_error = "The selected preview stage is out of range";
			return std::nullopt;
		}
		if (a_request.startStage > 0) {
			if (static_cast<std::size_t>(a_request.startStage) >= definition->linearStages.size()) {
				a_error = "The selected preview stage is out of range";
				return std::nullopt;
			}
			nodeId = definition->linearStages[a_request.startStage];
		}
		const auto* node = definition->FindNode(nodeId);
		if (!node) {
			a_error = "The selected preview node is no longer loaded";
			return std::nullopt;
		}

		std::vector<RE::Actor*> ordered;
		if (!Matchmaking::BindSceneRoles(*definition, a_request.actors, a_request.roleNames, ordered, a_error)) {
			return std::nullopt;
		}
		if (definition->RequiresAnchor() && !a_request.worldAnchor.set) {
			a_error = "This scene requires compatible furniture";
			return std::nullopt;
		}

		auto plan = Registry::ContentRegistry::GetSingleton().BuildNodePlan(definition, *node, ordered.size());
		if (!plan || plan->stages.empty()) {
			a_error = "The selected node has no playable preview";
			return std::nullopt;
		}
		for (auto& stage : plan->stages) {
			stage.timer = 0.0f;
			stage.loops = 0;
			// A preview is a transport over the whole clip: a `hold` stage would pin the clock and
			// make both the scrubber and PLAY inert. The browser shows what the clip contains; the
			// authored freeze is a scene-timing policy, not a property of the animation.
			stage.hold = -1.0f;
			stage.marks.clear();
		}
		plan->loopWhole = false;
		plan->speed = 0.0f;
		// A stage switch is a fresh Prepare while the outgoing preview still owns this cast (the
		// caller retires it between Prepare and Start). Its actors are already placed, so live
		// anchor/transform samples would describe the OUTGOING stage: inherit the first preview's
		// pre-inspection baseline instead. Only an identical cast inherits — a different binding
		// starts a genuinely new preview.
		const Preview* superseded = nullptr;
		for (const auto& [handle, preview] : _previews) {
			if (preview.participants == ordered) {
				superseded = &preview;
				break;
			}
		}

		bool anchorImplicit = false;
		if (a_request.worldAnchor.set) {
			plan->anchorExplicit = true;
			plan->anchorPos = a_request.worldAnchor.pos;
			plan->anchorHeading = a_request.worldAnchor.heading;
		} else if (plan->anchored) {
			// Preserve actor-relative placement while ensuring Layer A restores transforms on stop.
			anchorImplicit = true;
			plan->anchorExplicit = true;
			if (superseded && superseded->anchorImplicit) {
				plan->anchorPos = superseded->anchorPos;
				plan->anchorHeading = superseded->anchorHeading;
			} else {
				plan->anchorPos = ordered.front()->data.location;
				plan->anchorHeading = ordered.front()->data.angle.z;
			}
		}
		if (plan->anchored && plan->anchorExplicit) {
			if (superseded && superseded->baseline.size() == ordered.size()) {
				plan->baselineTransforms = superseded->baseline;
			} else {
				plan->baselineTransforms.reserve(ordered.size());
				for (const auto* actor : ordered) {
					plan->baselineTransforms.emplace_back(actor->data.location, actor->data.angle.z);
				}
			}
		}

		const auto stage = definition->LinearStageOf(nodeId);
		return PreparedPreview{
			definition, std::move(nodeId), stage, std::move(ordered), std::move(*plan), anchorImplicit
		};
	}

	std::optional<PreparedRoutePreview> PlaybackPreviewService::PrepareRoute(
		const RoutePreviewRequest& a_request, std::string& a_error) const
	{
		if (!a_request.definition) {
			a_error = "The selected route definition is no longer loaded";
			return std::nullopt;
		}
		if (!a_request.actor) {
			a_error = "The selected route preview actor is no longer available";
			return std::nullopt;
		}
		const auto* transition = a_request.definition->FindTransition(a_request.transition);
		if (!transition) {
			a_error = "The selected route transition is no longer loaded";
			return std::nullopt;
		}

		auto plan = Overlay::BuildRouteTransitionPreviewPlan(*a_request.definition, *transition);
		return PreparedRoutePreview{
			a_request.definition, transition->id, a_request.actor, std::move(plan)
		};
	}

	std::int32_t PlaybackPreviewService::Start(PreparedPreview a_prepared, std::string& a_error)
	{
		auto* owner = a_prepared.participants.empty() ? nullptr : a_prepared.participants.front();
		Animation::PlaybackId playbackId = 0;
		if (!Animation::GraphManager::GetSingleton().PlaySynchronized(
				a_prepared.participants, a_prepared.plan, 0, &playbackId)) {
			a_error = "The selected preview clip could not be loaded";
			return 0;
		}

		const auto handle = MintHandle();
		Preview preview;
		preview.handle = handle;
		preview.sceneId = a_prepared.definition->id;
		preview.node = std::move(a_prepared.node);
		preview.stage = a_prepared.stage;
		preview.definition = std::move(a_prepared.definition);
		preview.participants = std::move(a_prepared.participants);
		preview.playbackId = playbackId;
		preview.anchorImplicit = a_prepared.anchorImplicit;
		preview.anchorPos = a_prepared.plan.anchorPos;
		preview.anchorHeading = a_prepared.plan.anchorHeading;
		preview.baseline = std::move(a_prepared.plan.baselineTransforms);
		auto [inserted, ok] = _previews.emplace(handle, std::move(preview));
		if (!ok) {
			Animation::GraphManager::GetSingleton().StopScene(owner, playbackId);
			a_error = "Could not allocate a browser preview handle";
			return 0;
		}
		// The graph is live, so the clip length is already known — `atFrame` actions need it to place
		// themselves against the scrub position.
		float openedDuration = 0.0f;
		if (!inserted->second.participants.empty() && inserted->second.participants.front()) {
			if (const auto opened = Animation::GraphManager::GetSingleton().GetScenePlayback(
					inserted->second.participants.front(), playbackId)) {
				openedDuration = opened->duration;
			}
		}
		ReconcileProps(inserted->second, 0.0f, false, openedDuration);
		return handle;
	}

	std::int32_t PlaybackPreviewService::StartRoute(
		PreparedRoutePreview a_prepared, std::string& a_error)
	{
		if (!a_prepared.definition || !a_prepared.actor) {
			a_error = "The selected route preview is no longer available";
			return 0;
		}
		const auto handle = MintHandle();
		std::vector<RE::Actor*> participants{ a_prepared.actor };
		// A consumer route may already own the actor. Suspend it through the same blocker contract
		// production scenes use, then resume from its checkpoint when the preview is retired.
		Overlay::OverlayService::GetSingleton().SuspendForScene(handle, participants);

		Animation::PlaybackId playbackId = 0;
		if (!Animation::GraphManager::GetSingleton().PlaySynchronized(
				participants, a_prepared.plan, 0, &playbackId)) {
			Overlay::OverlayService::GetSingleton().ReconcileAfterScene(handle);
			a_error = "The selected route transition clip could not be loaded";
			return 0;
		}

		Preview preview;
		preview.handle = handle;
		preview.sceneId = a_prepared.definition->id;
		preview.route = std::move(a_prepared.definition);
		preview.transition = std::move(a_prepared.transition);
		preview.participants = std::move(participants);
		preview.playbackId = playbackId;
		preview.suspendsOverlay = true;
		auto [inserted, ok] = _previews.emplace(handle, std::move(preview));
		if (!ok) {
			Animation::GraphManager::GetSingleton().StopScene(a_prepared.actor, playbackId);
			Overlay::OverlayService::GetSingleton().ReconcileAfterScene(handle);
			a_error = "Could not allocate a route debugger preview handle";
			return 0;
		}
		ReconcileRouteProps(inserted->second, 0.0f, false);
		return handle;
	}

	RE::Actor* PlaybackPreviewService::RoleActor(const Preview& a_preview, std::string_view a_role) const
	{
		if (a_preview.participants.empty()) {
			return nullptr;
		}
		if (a_role.empty()) {
			return a_preview.participants.front();
		}
		const auto want = Util::ToLower(std::string(a_role));
		for (std::size_t i = 0; i < a_preview.definition->roles.size(); ++i) {
			if (Util::ToLower(a_preview.definition->roles[i].name) == want) {
				return i < a_preview.participants.size() ? a_preview.participants[i] : nullptr;
			}
		}
		return nullptr;
	}

	void PlaybackPreviewService::DestroyProps(Preview& a_preview)
	{
		auto& service = Props::PropService::GetSingleton();
		for (auto& prop : a_preview.props) {
			std::string error;
			if (!service.Destroy(prop.instance, &error)) {
				REX::ERROR("[UI] preview {} prop '{}' cleanup failed: {}", a_preview.handle, prop.id, error);
			}
		}
		a_preview.props.clear();
		for (auto& prop : a_preview.routeProps) {
			std::string error;
			if (!service.Destroy(prop.instance, &error)) {
				REX::ERROR("[UI] route preview {} prop '{}' cleanup failed: {}", a_preview.handle, prop.id, error);
			}
		}
		a_preview.routeProps.clear();
	}

	void PlaybackPreviewService::ReconcileRouteProps(
		Preview& a_preview, float a_frame, bool a_atEnd, float a_durationFrames)
	{
		const auto* transition = a_preview.route ? a_preview.route->FindTransition(a_preview.transition) : nullptr;
		if (!transition || a_preview.participants.empty() || !a_preview.participants.front()) return;
		const auto desired = InspectionRoutePropsAt(*transition, a_frame, a_atEnd, a_durationFrames);
		auto& service = Props::PropService::GetSingleton();

		for (auto it = a_preview.routeProps.begin(); it != a_preview.routeProps.end();) {
			const auto wanted = std::ranges::find_if(desired, [&](const Registry::RouteProp* a_prop) {
				return Util::ToLower(a_prop->id) == Util::ToLower(it->id);
			});
			if (wanted != desired.end()) {
				const auto* prop = *wanted;
				const bool unchanged = it->lifetime == prop->lifetime && it->source.kind == prop->source.kind &&
					it->source.form == prop->source.form && it->source.keywords == prop->source.keywords &&
					it->attachment.targetNode == prop->attachment.targetNode &&
					it->attachment.position == prop->attachment.position &&
					it->attachment.rotation == prop->attachment.rotation &&
					it->attachment.scale == prop->attachment.scale;
				if (unchanged) {
					++it;
					continue;
				}
			}
			std::string error;
			if (!service.Destroy(it->instance, &error)) {
				REX::ERROR("[UI] route preview {} prop '{}' destroy failed: {}", a_preview.handle, it->id, error);
			}
			it = a_preview.routeProps.erase(it);
		}

		for (const auto* prop : desired) {
			const auto live = std::ranges::find_if(a_preview.routeProps, [&](const PreviewRouteProp& a_live) {
				return Util::ToLower(a_live.id) == Util::ToLower(prop->id);
			});
			if (live != a_preview.routeProps.end()) continue;
			std::string error;
			auto instance = service.CreateAttached(
				a_preview.participants.front(), prop->source, prop->attachment, &error);
			if (instance.Empty()) {
				REX::WARN("[UI] route preview {} prop '{}' attach failed: {}", a_preview.handle, prop->id, error);
				continue;
			}
			a_preview.routeProps.push_back(PreviewRouteProp{
				prop->id, prop->lifetime, prop->source, prop->attachment, std::move(instance)
			});
		}
		REX::TRACE("[UI] route preview {} reconciled {} OSF-owned prop(s) at frame {:.3f}{}",
			a_preview.handle, desired.size(), a_frame, a_atEnd ? " (end)" : "");
	}

	void PlaybackPreviewService::ReconcileProps(Preview& a_preview, float a_fraction, bool a_atEnd,
		float a_durationSec)
	{
		const auto* node = a_preview.definition ? a_preview.definition->FindNode(a_preview.node) : nullptr;
		if (!node) {
			return;
		}
		const auto desired = InspectionPropsAt(node->actions, a_fraction, a_atEnd, a_durationSec);
		auto& service = Props::PropService::GetSingleton();

		for (auto it = a_preview.props.begin(); it != a_preview.props.end();) {
			const bool keep = std::ranges::any_of(desired,
				[&](const Registry::ActionEntry& a_action) { return a_action.prop == it->id; });
			if (keep) {
				++it;
				continue;
			}
			std::string error;
			if (!service.Destroy(it->instance, &error)) {
				REX::ERROR("[UI] preview {} prop '{}' destroy failed: {}", a_preview.handle, it->id, error);
			}
			it = a_preview.props.erase(it);
		}

		for (const auto& action : desired) {
			auto* actor = RoleActor(a_preview, action.role);
			if (!actor) {
				REX::WARN("[UI] preview {} prop '{}' role '{}' resolved no actor",
					a_preview.handle, action.prop, action.role);
				continue;
			}
			auto live = std::ranges::find_if(a_preview.props,
				[&](const PreviewProp& a_prop) { return a_prop.id == action.prop; });
			std::string error;
			if (live != a_preview.props.end()) {
				const auto& before = live->action;
				const bool unchanged = before.role == action.role &&
					before.propAttachment.targetNode == action.propAttachment.targetNode &&
					before.propAttachment.position == action.propAttachment.position &&
					before.propAttachment.rotation == action.propAttachment.rotation &&
					before.propAttachment.scale == action.propAttachment.scale;
				if (unchanged) {
					continue;
				}
				if (!service.Attach(live->instance, actor, action.propAttachment, &error)) {
					REX::WARN("[UI] preview {} prop '{}' reattach failed: {}", a_preview.handle, action.prop, error);
				} else {
					live->action = action;
				}
				continue;
			}
			auto created = service.CreateAttached(actor, action.propSource, action.propAttachment, &error);
			if (created.Empty()) {
				REX::WARN("[UI] preview {} prop '{}' create failed: {}", a_preview.handle, action.prop, error);
				continue;
			}
			a_preview.props.push_back(PreviewProp{ action.prop, action, std::move(created) });
		}
		REX::TRACE("[UI] preview {} reconciled {} prop(s) at {:.4f}{}", a_preview.handle,
			desired.size(), std::clamp(a_fraction, 0.0f, 1.0f), a_atEnd ? " (end)" : "");
	}

	bool PlaybackPreviewService::SetSpeed(std::int32_t a_handle, float a_speed)
	{
		const auto found = _previews.find(a_handle);
		if (found == _previews.end() || found->second.participants.empty() ||
			!found->second.participants.front() || !std::isfinite(a_speed) || a_speed < 0.0f) {
			return false;
		}
		auto& preview = found->second;
		auto& manager = Animation::GraphManager::GetSingleton();
		// Playback-id guarded: SetSpeed only knows about actors, and a retired preview's actor can
		// already be carrying a production scene by the time a stale view command lands.
		if (!manager.GetScenePlayback(preview.participants.front(), preview.playbackId)) {
			return false;
		}
		return manager.SetSpeed(preview.participants.front(), a_speed);
	}

	void PlaybackPreviewService::Tick()
	{
		auto& manager = Animation::GraphManager::GetSingleton();
		for (auto& [handle, preview] : _previews) {
			if (preview.participants.empty() || !preview.participants.front()) {
				continue;
			}
			const auto playback = manager.GetScenePlayback(preview.participants.front(), preview.playbackId);
			if (!playback || playback->speed <= 0.0f || playback->duration <= 0.0f) {
				continue;
			}
			if (preview.route) {
				ReconcileRouteProps(preview, playback->time * Registry::kFrameRate, /*a_atEnd*/ false,
					playback->duration * Registry::kFrameRate);
			} else {
				const float fraction = std::clamp(playback->time / playback->duration, 0.0f, 1.0f);
				ReconcileProps(preview, fraction, /*a_atEnd*/ false, playback->duration);
			}
		}
	}

	bool PlaybackPreviewService::Seek(std::int32_t a_handle, float a_time)
	{
		const auto found = _previews.find(a_handle);
		if (found == _previews.end() || found->second.participants.empty() ||
			!found->second.participants.front() || !std::isfinite(a_time)) {
			return false;
		}
		auto& preview = found->second;
		auto& manager = Animation::GraphManager::GetSingleton();
		const auto playback = manager.GetScenePlayback(preview.participants.front(), preview.playbackId);
		if (!playback || !manager.SetSceneTime(preview.participants.front(), a_time, preview.playbackId)) {
			return false;
		}
		if (playback->speed > 0.0f) {
			manager.SetSpeed(preview.participants.front(), 0.0f);  // scrubbing takes the transport
		}
		const bool atEnd = playback->duration > 0.0f && a_time >= playback->duration;
		if (preview.route) {
			ReconcileRouteProps(preview, std::max(0.0f, a_time) * Registry::kFrameRate, atEnd,
				playback->duration * Registry::kFrameRate);
		} else {
			const float fraction = playback->duration > 0.0f ?
				std::clamp(a_time / playback->duration, 0.0f, 1.0f) : 0.0f;
			ReconcileProps(preview, fraction, atEnd, playback->duration);
		}
		return true;
	}

	bool PlaybackPreviewService::Retire(std::int32_t a_handle, bool a_stopGraph)
	{
		const auto found = _previews.find(a_handle);
		if (found == _previews.end()) {
			return false;
		}
		Preview preview = std::move(found->second);
		_previews.erase(found);
		if (a_stopGraph && !preview.participants.empty()) {
			Animation::GraphManager::GetSingleton().StopScene(preview.participants.front(), preview.playbackId);
		}
		DestroyProps(preview);
		if (preview.suspendsOverlay) {
			Overlay::OverlayService::GetSingleton().ReconcileAfterScene(a_handle);
		}
		REX::DEBUG("[UI] stopped browser {} preview handle={}", preview.route ? "route" : "scene", a_handle);
		return true;
	}

	bool PlaybackPreviewService::Stop(std::int32_t a_handle)
	{
		return Retire(a_handle, true);
	}

	void PlaybackPreviewService::StopForActor(RE::Actor* a_actor)
	{
		std::vector<std::int32_t> handles;
		for (const auto& [handle, preview] : _previews) {
			if (std::ranges::find(preview.participants, a_actor) != preview.participants.end()) {
				handles.push_back(handle);
			}
		}
		for (const auto handle : handles) {
			Stop(handle);
		}
	}

	void PlaybackPreviewService::StopAll()
	{
		std::vector<std::int32_t> handles;
		handles.reserve(_previews.size());
		for (const auto& [handle, preview] : _previews) {
			(void)preview;
			handles.push_back(handle);
		}
		for (const auto handle : handles) {
			Stop(handle);
		}
	}

	bool PlaybackPreviewService::Contains(std::int32_t a_handle) const
	{
		return _previews.contains(a_handle);
	}

	std::vector<PreviewSnapshot> PlaybackPreviewService::List()
	{
		std::vector<PreviewSnapshot> result;
		std::vector<std::int32_t> stale;
		auto& manager = Animation::GraphManager::GetSingleton();
		for (const auto& [handle, preview] : _previews) {
			const auto playback = preview.participants.empty() ? std::nullopt :
				manager.GetScenePlayback(preview.participants.front(), preview.playbackId);
			if (!playback) {
				stale.push_back(handle);
				continue;
			}
			PreviewSnapshot snapshot;
			snapshot.handle = handle;
			snapshot.sceneId = preview.sceneId;
			snapshot.stage = preview.stage;
			snapshot.participants = preview.participants;
			snapshot.playback = *playback;
			if (preview.route) {
				snapshot.inspectionKind = "route";
				snapshot.routeId = preview.route->id;
				snapshot.transitionId = preview.transition;
			}
			result.push_back(std::move(snapshot));
		}
		for (const auto handle : stale) {
			Retire(handle, false);
		}
		return result;
	}
}
