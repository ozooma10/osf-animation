#pragma once

#include "Matchmaking/Matchmaker.h"
#include "Scene/SceneRuntime.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OSF::Scene
{
	// Keep in sync with OSFTypes.psc SceneOptions.
	struct LaunchOpts
	{
		RE::TESObjectREFR* anchor = nullptr;
		float              headingDeg = -1.0f;
		std::int32_t       stage = 0;
		float              speed = 1.0f;
		float              blendIn = 0.4f;
		std::int32_t       hideApparelMode = -1;     // tri-state: -1 inherit, 0 off, 1 on
		std::int32_t       playerInputLockMode = -1;
		std::int32_t       sceneControlsMode = -1;
		std::int32_t       fadeMode = -1;
		std::int32_t       followActorMode = -1;     // 1 = follow actors, 0 = anchor and pin
		std::string        camera;                  // "" = inherit; "none" = vanilla camera, suppresses node cameras
		float              loopScale = 1.0f;        // multiplies loop-driven stage loop counts
	};

	// nullopt when headingDeg < 0 (use the ref's own facing).
	std::optional<float> OptHeadingRad(const LaunchOpts& a_opts);

	SceneRuntime::AnchorOverride MakeAnchor(const LaunchOpts& a_opts);

	// Tri-states: 1 on, 0 off, else inherit. loopScale <= 0 or NaN -> 1.0, clamped to kLoopScaleMax.
	SceneRuntime::StartOverrides MakeOverrides(const LaunchOpts& a_opts);
	SceneRuntime::StartOverrides MakeOverrides(std::int32_t a_hideApparelMode, std::int32_t a_playerInputLockMode,
		std::int32_t a_sceneControlsMode, std::int32_t a_fadeMode, std::int32_t a_followActorMode,
		std::string_view a_camera, float a_loopScale);

	enum class InvalidStagePolicy { kReject, kUseEntry };

	struct DefLaunchOptions
	{
		SceneRuntime::StartOverrides overrides;
		RE::TESObjectREFR* anchorRef = nullptr;
		std::optional<float> headingRad;
		SceneRuntime::AnchorOverride worldAnchor;  // used only without a ref; cannot satisfy furniture requirements
		std::int32_t stage = 0;
		InvalidStagePolicy invalidStage = InvalidStagePolicy::kReject;
		std::optional<float> speed;  // seeds scene-owned speed before entry playback
		bool emitHud = true;
	};

	// Papyrus registered-scene options: Speed applies to the scene; BlendIn remains ad-hoc only.
	DefLaunchOptions MakeDefLaunchOptions(const LaunchOpts& a_opts);

	// Shared registered-scene entry for adapters and matchmaking. Game-thread call.
	// Null role names means positional cast; a supplied vector uses runtime role binding, even when empty.
	// a_logTag includes its bracketed subsystem tag and caller name.
	std::int32_t LaunchDef(std::string_view a_sceneId, const std::vector<RE::Actor*>& a_actors,
		const DefLaunchOptions& a_opts, const char* a_logTag, const std::vector<std::string>* a_roles = nullptr);

	// Matchmake, enforce the pick's anchor requirement, and start it. Returns the handle, 0 = failed.
	// a_logTag is the full log prefix; nothing is prepended.
	std::int32_t LaunchMatched(const std::vector<RE::Actor*>& a_actors,
		const Matchmaking::TagQuery& a_query, const LaunchOpts& a_opts,
		const char* a_logTag,
		Matchmaking::AnchorMode a_mode = Matchmaking::AnchorMode::kAllow);
}
