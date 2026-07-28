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

		struct RefTransform
		{
			RE::NiPoint3 pos;
			float        heading;  // radians, data.angle.z convention (forward = (-sin h, cos h))
		};

		// The transform the ref is actually RENDERED at. Furniture attached to a moving parent frame (ship interiors) keeps parent-LOCAL coordinates in data.location while the engine composes the attach chain into the render node
		// anchoring at data.location dumps the scene at the parent frame's origin ("actor teleports to world origin" on a ship). 
		// The 3D world transform is where the furniture is drawn, which is also the space the  compose-root pin stamps. Falls back to the logical transform when no 3D is loaded.
		RefTransform RenderedTransform(RE::TESObjectREFR* a_ref)
		{
			RefTransform logical{ a_ref->data.location, a_ref->data.angle.z };
			// Actor model roots are not reference roots: their composed 3D transform may be skeleton/
			// parent-relative and can be radically different from the position SetPosition expects.
			// Scene default anchoring already uses actor data.location, so reference-based player/NPC
			// locations must use that same coordinate space. The rendered fallback below is furniture-only.
			if (a_ref->IsActor()) {
				return logical;
			}

			RE::NiPointer<RE::NiAVObject> node;
			{
				const auto loaded = a_ref->loadedData.LockRead();
				if (*loaded) {
					node = (*loaded)->data3D;
				}
			}
			if (!node) {
				return logical;  // no 3D (scan/pick targets always have it; belt & braces)
			}

			const RE::NiTransform& world = node->world;
			// Same frame (normal worldspace furniture): keep the logical transform A real gap means an attached parent chain, where only the rendered transform matches what the player sees.
			constexpr float kSameFrameSq = 4.0f;  // 2 game units (~3 cm): transform jitter, not a frame gap
			if (logical.pos.GetSquaredDistance(world.translate) <= kSameFrameSq) {
				return logical;
			}

			// Heading from the composed rotation: where the node's model +Y (Creation forward) points in world, converted with the same convention the rest of OSF uses for data.angle.z (forward(h) = (-sin h, cos h);
			const RE::NiPoint3 fwd = world.rotate * RE::NiPoint3{ 0.0f, 1.0f, 0.0f };
			const RefTransform rendered{ world.translate, std::atan2(-fwd.x, fwd.y) };
			REX::DEBUG("[Scene] anchor ref {:#010x} is frame-attached — logical ({:.1f},{:.1f},{:.1f} h{:.2f}) -> rendered ({:.1f},{:.1f},{:.1f} h{:.2f})",
				a_ref->GetFormID(), logical.pos.x, logical.pos.y, logical.pos.z, logical.heading,
				rendered.pos.x, rendered.pos.y, rendered.pos.z, rendered.heading);
			return rendered;
		}

		std::optional<float> CurrentViewHeading()
		{
			auto* playerCamera = RE::PlayerCamera::GetSingleton();
			const auto cameraRoot = playerCamera ? playerCamera->cameraRoot : nullptr;
			if (!cameraRoot) {
				return std::nullopt;
			}

			// NiMatrix3 stores the camera's local basis by row: row 0 is +X/right and row 1 is
			// +Y/forward. Project forward onto the ground plane so looking up/down does not
			// change the requested distance.
			const float x = cameraRoot->world.rotate[1][0];
			const float y = cameraRoot->world.rotate[1][1];
			const float lengthSq = x * x + y * y;
			if (!std::isfinite(lengthSq) || lengthSq < 0.0001f) {
				return std::nullopt;
			}
			return std::atan2(-x, y);
		}
	}

	SceneRuntime::AnchorOverride MakeAnchorAt(RE::TESObjectREFR* a_ref, std::optional<float> a_headingRad)
	{
		SceneRuntime::AnchorOverride anchor{};
		if (a_ref) {
			const RefTransform base = RenderedTransform(a_ref);
			anchor.set = true;
			anchor.pos = base.pos;
			anchor.heading = a_headingRad ? *a_headingRad : base.heading;
		}
		return anchor;
	}

	SceneRuntime::AnchorOverride MakeAnchorInFrontOfView(RE::TESObjectREFR* a_ref, float a_distance)
	{
		if (!a_ref || !std::isfinite(a_distance)) {
			return {};
		}
		const RefTransform base = RenderedTransform(a_ref);
		const std::optional<float> viewHeading = CurrentViewHeading();
		const float heading = viewHeading.value_or(base.heading);
		const RE::NiPoint3 pos{
			base.pos.x - std::sin(heading) * a_distance,
			base.pos.y + std::cos(heading) * a_distance,
			base.pos.z
		};

		REX::DEBUG("[Scene] front-of-view anchor ref {:#010x}: origin ({:.1f},{:.1f},{:.1f}) actorHeading {:.2f}, "
			"viewHeading {:.2f}{} distance {:.1f} -> ({:.1f},{:.1f},{:.1f})",
			a_ref->GetFormID(), base.pos.x, base.pos.y, base.pos.z, base.heading, heading,
			viewHeading ? "" : " (actor fallback)", a_distance, pos.x, pos.y, pos.z);
		return SceneRuntime::AnchorOverride{ true, pos, heading };
	}

	std::optional<SceneRuntime::AnchorOverride> ResolveSceneAnchor(
		std::string_view a_sceneId, RE::TESObjectREFR* a_ref, std::optional<float> a_headingRad, bool a_emitHud)
	{
		const auto def = Registry::SceneRegistry::GetSingleton().Find(a_sceneId);
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
		// Base ref transform: its RENDERED origin + facing (an explicit heading override is honored).
		const RefTransform base = RenderedTransform(a_ref);
		const float        baseHeading = a_headingRad ? *a_headingRad : base.heading;
		return ComposeAnchor(base.pos, baseHeading, def->anchorOffset);
	}
}
