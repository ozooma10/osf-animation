#pragma once

#include "Scene/SceneRuntime.h"

#include <optional>
#include <string_view>

namespace OSF::Scene
{
	// No scene validation; passes the ref transform through.
	SceneRuntime::AnchorOverride MakeAnchorAt(RE::TESObjectREFR* a_ref, std::optional<float> a_headingRad);

	// Query the nearest navmesh position; the caller owns distance/height acceptance policy.
	std::optional<RE::NiPoint3> SnapWorldPosition(RE::TESObjectREFR* ref, const RE::NiPoint3& position);

	// nullopt = anchor-bound scene with a missing or unaccepted ref; abort the start.
	std::optional<SceneRuntime::AnchorOverride> ResolveSceneAnchor(std::string_view a_sceneId, RE::TESObjectREFR* a_ref, std::optional<float> a_headingRad, bool a_emitHud);
}
