#include "Scene/SceneLauncher.h"

#include "Matchmaking/Matchmaker.h"  // Pick (matchmaking)
#include "Scene/AnchorResolve.h"     // MakeAnchorAt + ResolveSceneAnchor
#include "Scene/SceneRuntime.h"
#include "UI/HudMessage.h"
#include "Util/Math.h"  // kDegToRadF

#include <format>

namespace OSF::Scene
{
	namespace
	{
		// Upper bound for LoopScale
		constexpr float kLoopScaleMax = 20.0f;

		// Start a matchmade candidate using its resolved binding (Matchmaking::Pick already chose the slot->actor order, so we never re-bind here) at an already-resolved anchor.
		// Binds by declaration order = the reordered actors. anchor unset => co-located at actor[0]; set => world-anchored.
		std::int32_t StartCandidate(const Matchmaking::Candidate& a_pick, const std::vector<RE::Actor*>& a_actors,
			const SceneRuntime::AnchorOverride& a_anchor, const SceneRuntime::StartOverrides& a_over)
		{
			std::vector<RE::Actor*> ordered(a_pick.order.size());
			for (size_t slot = 0; slot < a_pick.order.size(); slot++) {
				ordered[slot] = a_actors[a_pick.order[slot]];
			}
			auto& rt = SceneRuntime::GetSingleton();
			if (a_anchor.set) {
				return rt.StartFromDefAt(a_pick.id, ordered, a_anchor.pos, a_anchor.heading, a_over);
			}
			return rt.StartFromDef(a_pick.id, ordered, a_over);
		}
	}

	// Optional explicit heading (radians) from options: HeadingDeg < 0 => use the ref's own facing.
	std::optional<float> OptHeadingRad(const LaunchOpts& a_opts)
	{
		return (a_opts.headingDeg < 0.0f) ? std::nullopt : std::optional<float>(a_opts.headingDeg * Util::kDegToRadF);
	}

	// A SceneRuntime world-anchor from resolved options (unset when no Anchor).
	SceneRuntime::AnchorOverride MakeAnchor(const LaunchOpts& a_opts)
	{
		return MakeAnchorAt(a_opts.anchor, OptHeadingRad(a_opts));
	}

	// SceneRuntime per-start overrides from resolved options. Tri-state ints map to optional<bool> (1 = on, 0 = off, anything else incl. -1 = inherit the scene's pack default).
	// LoopScale is sanitized: <=0 or NaN -> 1.0 (no scaling); inf / overshoot -> clamped to kLoopScaleMax.
	SceneRuntime::StartOverrides MakeOverrides(const LaunchOpts& a_opts)
	{
		return MakeOverrides(a_opts.stripMode, a_opts.lockPlayerMode, a_opts.playerControlMode,
			a_opts.fadeMode, a_opts.inPlaceMode, a_opts.camera, a_opts.loopScale);
	}

	SceneRuntime::StartOverrides MakeOverrides(std::int32_t a_stripMode, std::int32_t a_lockPlayerMode,
		std::int32_t a_playerControlMode, std::int32_t a_fadeMode, std::int32_t a_inPlaceMode,
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
			return std::nullopt;  // -1 and any out-of-range value = inherit
		};
		over.strip = triState(a_stripMode);
		over.lockPlayer = triState(a_lockPlayerMode);
		over.playerControl = triState(a_playerControlMode);
		over.fade = triState(a_fadeMode);
		over.inPlace = triState(a_inPlaceMode);
		if (!a_camera.empty()) {
			over.camera = std::string(a_camera);
		}
		float ls = a_loopScale;
		if (!(ls > 0.0f)) {  // false for <=0 AND for NaN -> no-op
			ls = 1.0f;
		} else if (ls > kLoopScaleMax) {
			ls = kLoopScaleMax;
		}
		over.loopScale = ls;
		return over;
	}

	std::int32_t LaunchMatched(const std::vector<RE::Actor*>& a_actors, const Matchmaking::TagQuery& a_query, const LaunchOpts& a_opts, const SceneRuntime::StartOverrides& a_over, const char* a_logTag, Matchmaking::AnchorMode a_mode)
	{
		if (a_actors.empty()) {
			REX::DEBUG("{}: no actors given", a_logTag);
			return 0;
		}
		for (auto* actor : a_actors) {
			if (!actor) {
				REX::DEBUG("{}: null actor in list", a_logTag);
				return 0;
			}
		}
		auto pick = Matchmaking::Pick(a_actors, a_query, a_opts.anchor, a_mode);
		if (!pick) {
			REX::DEBUG("{}: no matching animation found for the given tags/actors", a_logTag);
			UI::HudMessage::Error("no matching animation for the given tags/actors");
			return 0;
		}
		const auto anchor = ResolveSceneAnchor(pick->id, a_opts.anchor, OptHeadingRad(a_opts), /*a_emitHud*/ true);
		if (!anchor) {
			return 0;  // anchor-bound pick without a compatible anchor (logged in ResolveSceneAnchor)
		}
		const std::int32_t handle = StartCandidate(*pick, a_actors, *anchor, a_over);
		if (handle) {
			REX::INFO("{}: playing '{}' handle {:#010x}{}", a_logTag, pick->id, handle, anchor->set ? " (anchored)" : "");
		} else {
			REX::WARN("{}: could not start matched scene '{}'", a_logTag, pick->id);
			UI::HudMessage::Error(std::format("could not start scene '{}'", pick->id));
		}
		return handle;
	}
}
