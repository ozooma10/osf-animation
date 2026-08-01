#include "Scene/SceneInspectionService.h"

#include "Matchmaking/Matchmaker.h"
#include "Scene/InspectionPropTimeline.h"
#include "Util/StringUtil.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace OSF::Scene
{
	SceneInspectionService& SceneInspectionService::GetSingleton()
	{
		static SceneInspectionService singleton;
		return singleton;
	}

	std::optional<PreparedInspection> SceneInspectionService::Prepare(
		const InspectionRequest& a_request, std::string& a_error) const
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
		if (definition->RequiresAnchor() && !a_request.anchor.set) {
			a_error = "This scene requires compatible furniture";
			return std::nullopt;
		}

		auto plan = Registry::SceneRegistry::GetSingleton().BuildNodePlan(definition, *node, ordered.size());
		if (!plan || plan->stages.empty()) {
			a_error = "The selected node has no playable preview";
			return std::nullopt;
		}
		for (auto& stage : plan->stages) {
			stage.timer = 0.0f;
			stage.loops = 0;
			stage.marks.clear();
		}
		plan->loopWhole = false;
		plan->speed = 0.0f;
		if (a_request.anchor.set) {
			plan->anchorExplicit = true;
			plan->anchorPos = a_request.anchor.pos;
			plan->anchorHeading = a_request.anchor.heading;
		} else if (plan->anchored) {
			// Preserve actor-relative placement while ensuring Layer A restores transforms on stop.
			plan->anchorExplicit = true;
			plan->anchorPos = ordered.front()->data.location;
			plan->anchorHeading = ordered.front()->data.angle.z;
		}

		const auto stage = definition->LinearStageOf(nodeId);
		return PreparedInspection{ definition, std::move(nodeId), stage, std::move(ordered), std::move(*plan) };
	}

	std::int32_t SceneInspectionService::Start(PreparedInspection a_prepared, std::string& a_error)
	{
		auto* owner = a_prepared.participants.empty() ? nullptr : a_prepared.participants.front();
		Animation::PlaybackId playbackId = 0;
		if (!Animation::GraphManager::GetSingleton().PlaySceneStaged(
				a_prepared.participants, a_prepared.plan, 0, &playbackId)) {
			a_error = "The selected preview clip could not be loaded";
			return 0;
		}

		while (_nextHandle == 0 || _previews.contains(_nextHandle)) {
			_nextHandle = _nextHandle == (std::numeric_limits<std::int32_t>::min)() ? -1 : _nextHandle - 1;
		}
		const auto handle = _nextHandle;
		_nextHandle = _nextHandle == (std::numeric_limits<std::int32_t>::min)() ? -1 : _nextHandle - 1;
		Preview preview{
			handle, a_prepared.definition->id, std::move(a_prepared.node), a_prepared.stage,
			std::move(a_prepared.definition), std::move(a_prepared.participants), playbackId, {}
		};
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

	RE::Actor* SceneInspectionService::RoleActor(const Preview& a_preview, std::string_view a_role) const
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

	void SceneInspectionService::DestroyProps(Preview& a_preview)
	{
		auto& service = Props::PropService::GetSingleton();
		for (auto& prop : a_preview.props) {
			std::string error;
			if (!service.Destroy(prop.instance, &error)) {
				REX::ERROR("[UI] preview {} prop '{}' cleanup failed: {}", a_preview.handle, prop.id, error);
			}
		}
		a_preview.props.clear();
	}

	void SceneInspectionService::ReconcileProps(Preview& a_preview, float a_fraction, bool a_atEnd,
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
					before.propAttachment.node == action.propAttachment.node &&
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

	bool SceneInspectionService::Seek(std::int32_t a_handle, float a_time)
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
		const bool atEnd = playback->duration > 0.0f && a_time >= playback->duration;
		const float fraction = playback->duration > 0.0f ?
			std::clamp(a_time / playback->duration, 0.0f, 1.0f) : 0.0f;
		ReconcileProps(preview, fraction, atEnd, playback->duration);
		return true;
	}

	bool SceneInspectionService::Retire(std::int32_t a_handle, bool a_stopGraph)
	{
		const auto found = _previews.find(a_handle);
		if (found == _previews.end()) {
			return false;
		}
		Preview preview = std::move(found->second);
		_previews.erase(found);
		DestroyProps(preview);
		if (a_stopGraph && !preview.participants.empty()) {
			Animation::GraphManager::GetSingleton().StopScene(preview.participants.front(), preview.playbackId);
		}
		REX::DEBUG("[UI] stopped browser prop preview handle={}", a_handle);
		return true;
	}

	bool SceneInspectionService::Stop(std::int32_t a_handle)
	{
		return Retire(a_handle, true);
	}

	void SceneInspectionService::StopForActor(RE::Actor* a_actor)
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

	void SceneInspectionService::StopAll()
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

	bool SceneInspectionService::Contains(std::int32_t a_handle) const
	{
		return _previews.contains(a_handle);
	}

	std::vector<InspectionSnapshot> SceneInspectionService::List()
	{
		std::vector<InspectionSnapshot> result;
		std::vector<std::int32_t> stale;
		auto& manager = Animation::GraphManager::GetSingleton();
		for (const auto& [handle, preview] : _previews) {
			const auto playback = preview.participants.empty() ? std::nullopt :
				manager.GetScenePlayback(preview.participants.front(), preview.playbackId);
			if (!playback) {
				stale.push_back(handle);
				continue;
			}
			result.push_back(InspectionSnapshot{
				handle, preview.sceneId, preview.stage, preview.participants, *playback
			});
		}
		for (const auto handle : stale) {
			Retire(handle, false);
		}
		return result;
	}
}
