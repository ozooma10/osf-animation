#pragma once

#include "Scene/SceneRuntime.h"  // SceneRuntime::AnchorOverride

#include <optional>
#include <string_view>

//world anchor resolution for scenes. Determines where scenes anchor and if furniture is allowed.
namespace OSF::Scene
{
	// World anchor at a ref with no scene validation. Basically passes through ref transform
	SceneRuntime::AnchorOverride MakeAnchorAt(RE::TESObjectREFR* a_ref, std::optional<float> a_headingRad);

	// World anchor a_distance game units in front of a reference, using its rendered transform so
	// actors aboard attached ship/interior frames resolve in the same world space as furniture.
	SceneRuntime::AnchorOverride MakeAnchorInFrontOf(RE::TESObjectREFR* a_ref, float a_distance);

	// Resolve and enforce a_sceneId scenes anchor requirement at a_ref.
	// For anchor bound scenes, ref must be accepted anchor, returns nullopt to abort start if ref is missing or doesnt fit anchor requirements
	// a_emitHud is if warning/error should be emitted to user
	std::optional<SceneRuntime::AnchorOverride> ResolveSceneAnchor(std::string_view a_sceneId, RE::TESObjectREFR* a_ref, std::optional<float> a_headingRad, bool a_emitHud);
}
