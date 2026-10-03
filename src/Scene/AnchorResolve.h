#pragma once

#include "Scene/SceneRuntime.h"

#include <optional>
#include <string_view>

namespace OSF::Scene
{
	// No scene validation; passes the ref transform through.
	SceneRuntime::AnchorOverride MakeAnchorAt(RE::TESObjectREFR* a_ref, std::optional<float> a_headingRad);

	// a_distance meters along the camera's horizontal look direction (ref heading if no camera).
	SceneRuntime::AnchorOverride MakeAnchorInFrontOfView(RE::TESObjectREFR* a_ref, float a_distance);

	// nullopt = anchor-bound scene with a missing or unaccepted ref; abort the start.
	std::optional<SceneRuntime::AnchorOverride> ResolveSceneAnchor(std::string_view a_sceneId, RE::TESObjectREFR* a_ref, std::optional<float> a_headingRad, bool a_emitHud);
}
