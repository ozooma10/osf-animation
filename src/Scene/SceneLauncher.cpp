#include "Scene/SceneLauncher.h"

#include "Animation/GraphManager.h"
#include "Matchmaking/Matchmaker.h"
#include "Scene/AnchorResolve.h"
#include "Scene/SceneRuntime.h"
#include "UI/HudMessage.h"
#include "Util/Math.h"

#include <format>

namespace OSF::Scene
{
	namespace
	{
		constexpr float kLoopScaleMax = 20.0f;

		bool ValidActors(const std::vector<RE::Actor*>& a_actors, const char* a_logTag)
		{
			if (a_actors.empty()) {
				REX::DEBUG("{}: no actors given", a_logTag);
				return false;
			}
			for (auto* actor : a_actors) {
				if (!actor) {
					REX::DEBUG("{}: null actor in list", a_logTag);
					return false;
				}
			}
			return true;
		}

		std::optional<std::string> ResolveStageEntry(const Registry::SceneDef& a_def,
			const DefLaunchOptions& a_opts, const char* a_logTag)
		{
			if (a_opts.stage <= 0) {
				return std::string{};
			}
			if (a_def.linearStages.empty()) {
				REX::WARN("{}: scene '{}' has no linear stages — stage {} ignored", a_logTag, a_def.id, a_opts.stage);
				return std::string{};
			}
			if (a_opts.stage >= static_cast<std::int32_t>(a_def.linearStages.size())) {
				REX::WARN("{}: stage {} out of range for scene '{}' ({} stages)",
					a_logTag, a_opts.stage, a_def.id, a_def.linearStages.size());
				if (a_opts.invalidStage == InvalidStagePolicy::kUseEntry) {
					return std::string{};
				}
				if (a_opts.emitHud) {
					UI::HudMessage::Error(std::format("scene '{}' has no stage {}", a_def.id, a_opts.stage));
				}
				return std::nullopt;
			}
			return a_def.linearStages[a_opts.stage];
		}
	}

	std::optional<float> OptHeadingRad(const LaunchOpts& a_opts)
	{
		return (a_opts.headingDeg < 0.0f) ? std::nullopt : std::optional<float>(a_opts.headingDeg * Util::kDegToRadF);
	}

	SceneRuntime::AnchorOverride MakeAnchor(const LaunchOpts& a_opts)
	{
		return MakeAnchorAt(a_opts.anchor, OptHeadingRad(a_opts));
	}

	SceneRuntime::StartOverrides MakeOverrides(const LaunchOpts& a_opts)
	{
		return MakeOverrides(a_opts.hideApparelMode, a_opts.playerInputLockMode, a_opts.sceneControlsMode,
			a_opts.fadeMode, a_opts.followActorMode, a_opts.camera, a_opts.loopScale);
	}

	SceneRuntime::StartOverrides MakeOverrides(std::int32_t a_hideApparelMode, std::int32_t a_playerInputLockMode,
		std::int32_t a_sceneControlsMode, std::int32_t a_fadeMode, std::int32_t a_followActorMode,
		std::string_view a_camera, float a_loopScale)
	{
		SceneRuntime::StartOverrides over{};
		const auto triState = [](std::int32_t a_v) -> std::optional<bool> {
			if (a_v == 1) {
				return true;
			}
			if (a_v == 0) {
				return false;
			}
			return std::nullopt;
		};
		over.hideApparel = triState(a_hideApparelMode);
		over.playerInputLock = triState(a_playerInputLockMode);
		over.sceneControls = triState(a_sceneControlsMode);
		over.fade = triState(a_fadeMode);
		if (const auto followActor = triState(a_followActorMode)) {
			over.worldPlacement = Animation::WorldPlacementFromFollowActor(*followActor);
		}
		if (!a_camera.empty()) {
			over.camera = std::string(a_camera);
		}
		float ls = a_loopScale;
		if (!(ls > 0.0f)) {  // also catches NaN
			ls = 1.0f;
		} else if (ls > kLoopScaleMax) {
			ls = kLoopScaleMax;
		}
		over.loopScale = ls;
		return over;
	}

	DefLaunchOptions MakeDefLaunchOptions(const LaunchOpts& a_opts)
	{
		DefLaunchOptions launch;
		launch.overrides = MakeOverrides(a_opts);
		launch.anchorRef = a_opts.anchor;
		launch.headingRad = OptHeadingRad(a_opts);
		launch.stage = a_opts.stage;
		launch.speed = a_opts.speed;
		return launch;
	}

	std::int32_t LaunchDef(std::string_view a_sceneId, const std::vector<RE::Actor*>& a_actors,
		const DefLaunchOptions& a_opts, const char* a_logTag, const std::vector<std::string>* a_roles)
	{
		if (!ValidActors(a_actors, a_logTag)) {
			return 0;
		}
		const auto def = Registry::ContentRegistry::GetSingleton().Find(a_sceneId);
		if (!def) {
			REX::ERROR("{}: no scene '{}'", a_logTag, a_sceneId);
			return 0;
		}
		const auto entryNode = ResolveStageEntry(*def, a_opts, a_logTag);
		if (!entryNode) {
			return 0;
		}
		auto anchor = a_opts.worldAnchor;
		if (a_opts.anchorRef || def->RequiresAnchor()) {
			const auto resolved = ResolveSceneAnchor(a_sceneId, a_opts.anchorRef, a_opts.headingRad, a_opts.emitHud);
			if (!resolved) {
				return 0;
			}
			anchor = *resolved;
		}
		auto& rt = SceneRuntime::GetSingleton();
		auto overrides = a_opts.overrides;
		if (a_opts.speed) overrides.speed = a_opts.speed;
		std::int32_t handle;
		if (a_roles) {
			handle = rt.StartFromDefRoles(a_sceneId, a_actors, *a_roles, anchor, overrides, *entryNode);
		} else if (anchor.set) {
			handle = rt.StartFromDefAt(a_sceneId, a_actors, anchor.pos, anchor.heading, overrides, *entryNode);
		} else {
			handle = rt.StartFromDef(a_sceneId, a_actors, overrides, *entryNode);
		}
		return handle;
	}

	std::int32_t LaunchMatched(const std::vector<RE::Actor*>& a_actors, const Matchmaking::TagQuery& a_query,
		const LaunchOpts& a_opts, const char* a_logTag, Matchmaking::AnchorMode a_mode)
	{
		if (!ValidActors(a_actors, a_logTag)) {
			return 0;
		}
		auto pick = Matchmaking::Pick(a_actors, a_query, a_opts.anchor, a_mode);
		if (!pick) {
			REX::DEBUG("{}: no matching animation found for the given tags/actors", a_logTag);
			UI::HudMessage::Error("no matching animation for the given tags/actors");
			return 0;
		}
		std::vector<RE::Actor*> ordered(pick->order.size());
		for (size_t slot = 0; slot < pick->order.size(); slot++) {
			ordered[slot] = a_actors[pick->order[slot]];
		}
		auto launch = MakeDefLaunchOptions(a_opts);
		launch.stage = 0;  // Matchmade starts have always entered at the authored entry.
		const std::int32_t handle = LaunchDef(pick->id, ordered, launch, a_logTag);
		if (handle) {
			REX::DEBUG("{}: matched '{}' handle {:#010x}{}", a_logTag, pick->id, handle, a_opts.anchor ? " (anchored)" : "");
		} else {
			REX::WARN("{}: could not start matched scene '{}'", a_logTag, pick->id);
			UI::HudMessage::Error(std::format("could not start scene '{}'", pick->id));
		}
		return handle;
	}
}
