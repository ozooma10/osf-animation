#include "Scene/AnchorResolve.h"

#include "Animation/Scene.h"         // ParticipantPlacement + PlacementToWorld (anchor-offset composition)
#include "Matchmaking/Matchmaker.h"  // AnchorAccepts (furniture validation)
#include "Registry/SceneRegistry.h"  // SceneDef::RequiresAnchor / anchorOffset
#include "UI/HudMessage.h"

#include <format>

namespace OSF::Scene
{
	namespace
	{
		// Compose the scene's anchorOffset onto a base ref transform (pos, heading in radians) 
		// rotate x/y into the ref's heading frame (PlacementToWorld) and add the offset heading.
		SceneRuntime::AnchorOverride ComposeAnchor(RE::NiPoint3 a_basePos, float a_baseHeading,
			const Animation::ParticipantPlacement& a_offset)
		{
			RE::NiPoint3 pos = Animation::PlacementToWorld(a_basePos, a_baseHeading, a_offset);
			return SceneRuntime::AnchorOverride{ true, pos, a_baseHeading + a_offset.heading };
		}
	}

	SceneRuntime::AnchorOverride MakeAnchorAt(RE::TESObjectREFR* a_ref, std::optional<float> a_headingRad)
	{
		SceneRuntime::AnchorOverride anchor{};
		if (a_ref) {
			anchor.set = true;
			anchor.pos = a_ref->data.location;
			anchor.heading = a_headingRad ? *a_headingRad : a_ref->data.angle.z;
		}
		return anchor;
	}

	std::optional<SceneRuntime::AnchorOverride> ResolveSceneAnchor(
		std::string_view a_sceneId, RE::TESObjectREFR* a_ref, std::optional<float> a_headingRad, bool a_emitHud)
	{
		const auto* def = Registry::SceneRegistry::GetSingleton().Find(a_sceneId);
		if (!def || !def->RequiresAnchor()) {
			return MakeAnchorAt(a_ref, a_headingRad);  // free scene: anchor optional, pass through
		}
		if (!a_ref) {
			REX::WARN("[Scene] scene '{}' is anchor-bound but no anchor ref was supplied — start aborted", a_sceneId);
			if (a_emitHud) {
				UI::HudMessage::Error(std::format("scene '{}' needs a furniture anchor to play", a_sceneId));
			}
			return std::nullopt;
		}
		if (!Matchmaking::AnchorAccepts(*def, a_ref)) {
			REX::WARN("[Scene] scene '{}' anchor ref {:#010x} isn't the furniture this scene requires — start aborted",
				a_sceneId, a_ref->GetFormID());
			if (a_emitHud) {
				UI::HudMessage::Error("that object isn't the right furniture for this scene");
			}
			return std::nullopt;
		}
		// Base ref transform: its origin + facing (an explicit heading override is honored).
		const RE::NiPoint3 basePos = a_ref->data.location;
		const float        baseHeading = a_headingRad ? *a_headingRad : a_ref->data.angle.z;
		return ComposeAnchor(basePos, baseHeading, def->anchorOffset);
	}
}
