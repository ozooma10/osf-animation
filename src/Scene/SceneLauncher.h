#pragma once

#include "Matchmaking/Matchmaker.h"  // TagQuery, AnchorMode, Candidate
#include "Scene/SceneRuntime.h"      // AnchorOverride, StartOverrides

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The matchmake-and-start funnel shared by every launch path (Papyrus natives, hotkeys, director).
namespace OSF::Scene
{
	// Per-start options shared by every launch path. Keep in sync with OSFTypes.psc SceneOptions.
	struct LaunchOpts
	{
		RE::TESObjectREFR* anchor = nullptr;
		float              headingDeg = -1.0f;
		std::int32_t       stage = 0;
		float              speed = 1.0f;
		float              blendIn = 0.4f;
		std::int32_t       stripMode = -1;          // tri-state override: -1 inherit, 0 off, 1 on
		std::int32_t       lockPlayerMode = -1;     // tri-state override
		std::int32_t       playerControlMode = -1;  // tri-state override of the director-input grant (OFF = no advance/end)
		std::int32_t       fadeMode = -1;           // tri-state override
		std::int32_t       inPlaceMode = -1;        // tri-state override: 1 = no teleport / per-frame root+heading pin (rig follows the actor)
		std::string        camera;                  // camera state override ("" = inherit; "none" = leave the vanilla camera alone; suppresses authored node cameras)
		float              loopScale = 1.0f;        // multiply loop-driven stage loop counts (1.0 = none)
	};

	// Optional explicit heading (radians) from options: HeadingDeg < 0 => use the ref's own facing.
	std::optional<float> OptHeadingRad(const LaunchOpts& a_opts);

	// A SceneRuntime world-anchor from resolved options (unset when no Anchor).
	SceneRuntime::AnchorOverride MakeAnchor(const LaunchOpts& a_opts);

	// SceneRuntime per-start overrides from resolved options. Tri-state ints map to optional<bool> (1 = on, 0 = off, anything else incl. -1 = inherit the scene's pack default).
	// LoopScale is sanitized: <=0 or NaN -> 1.0 (no scaling); inf / overshoot -> clamped to kLoopScaleMax.
	SceneRuntime::StartOverrides MakeOverrides(const LaunchOpts& a_opts);

	// Validate the actor list, matchmake a_query across the scene registry (priority tier + weighted-random) with anchor filtering (a_mode + a_opts.anchor),
	// ENFORCE the picked scene's anchor requirement (ResolveSceneAnchor), and start the pick with its matchmade binding at the resolved anchor.
	// Returns the scene handle (0 = no actors / null actor / no match / anchor rejected / start failed).
	// a_logTag carries the FULL log prefix including the caller's subsystem tag, e.g. "[Papyrus] OSF.StartSceneByTags" — this funnel prepends nothing.
	std::int32_t LaunchMatched(const std::vector<RE::Actor*>& a_actors,
		const Matchmaking::TagQuery& a_query, const LaunchOpts& a_opts,
		const SceneRuntime::StartOverrides& a_over, const char* a_logTag,
		Matchmaking::AnchorMode a_mode = Matchmaking::AnchorMode::kAllow);
}
