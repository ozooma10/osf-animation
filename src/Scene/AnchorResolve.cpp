#include "Scene/AnchorResolve.h"

#include "Animation/Scene.h"
#include "Matchmaking/Matchmaker.h"
#include "Registry/ContentRegistry.h"
#include "UI/HudMessage.h"

#include <atomic>
#include <cstdint>
#include <format>

namespace OSF::Scene
{
	namespace
	{
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

		// Ship-interior furniture keeps parent-local data.location; anchoring there puts the scene at the
		// ship's origin, so use the rendered world transform when it differs.
		RefTransform RenderedTransform(RE::TESObjectREFR* a_ref)
		{
			RefTransform logical{ a_ref->data.location, a_ref->data.angle.z };
			// Actor 3D roots can be skeleton-relative; actors stay in data.location space.
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
				return logical;
			}

			const RE::NiTransform& world = node->world;
			constexpr float kSameFrameSq = 4.0f;  // 2 game units: jitter, not a parent-frame gap
			if (logical.pos.GetSquaredDistance(world.translate) <= kSameFrameSq) {
				return logical;
			}

			// Model +Y is forward.
			const RE::NiPoint3 fwd = world.rotate * RE::NiPoint3{ 0.0f, 1.0f, 0.0f };
			const RefTransform rendered{ world.translate, std::atan2(-fwd.x, fwd.y) };
			REX::DEBUG("[Scene] anchor ref {:#010x} is frame-attached — logical ({:.1f},{:.1f},{:.1f} h{:.2f}) -> rendered ({:.1f},{:.1f},{:.1f} h{:.2f})",
				a_ref->GetFormID(), logical.pos.x, logical.pos.y, logical.pos.z, logical.heading,
				rendered.pos.x, rendered.pos.y, rendered.pos.z, rendered.heading);
			return rendered;
		}

		// Navmesh snap mirrors GameScript::MoveToNearestNavmeshLocFunctor slot 7 (OSF RE world.navmesh_query):
		// FindTriangleForLocation clamps laterally and vertically. Positions are in data.location space.
		struct PathingPoint
		{
			RE::NiPoint3  pos;
			std::uint32_t spaceIdx;  // index into the rebase table; >= count = absolute coords
			RE::TESForm*  ctx;       // REQUIRED cell/worldspace — the space resolver keys on it
		};
		static_assert(sizeof(PathingPoint) == 0x18);

		struct PathingLocation
		{
			RE::NiPoint3  pos;
			std::uint32_t spaceIdx;
			void*         navmeshRef;   // refcounted, written by the find on success
			void*         spaceObject;  // refcounted, written by CreatePathingLocation
			std::uint64_t poly;
			std::uint8_t  state;
			std::uint8_t  flags;
			std::uint8_t  pad2A[6];
		};
		static_assert(sizeof(PathingLocation) == 0x30);

		struct TraversableFilter  // FindTriangleForLocationTraversableFilter, stack-constructed
		{
			const void*   vptr{ nullptr };
			float         zAboveExtent{ 0.0f };  // consulted only when radius >= 0
			float         zBelowExtent{ 1.0f };
			float         radius{ -1.0f };  // negative = unlimited (bbox pre-check short-circuits)
			std::uint32_t pad14{ 0 };
			std::uint8_t  flag{ 1 };
			std::uint8_t  pad19[7]{};
		};
		static_assert(sizeof(TraversableFilter) == 0x20);

		// Engine release pattern: refcount at +8; on last ref call vtbl slot 0 (deleting dtor, flag 1).
		void ReleasePathingRef(void* a_ptr)
		{
			if (!a_ptr) {
				return;
			}
			auto& count = *reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uintptr_t>(a_ptr) + 8);
			if (std::atomic_ref<std::int32_t>(count).fetch_sub(1) == 1) {
				using Dtor = void* (*)(void*, std::uint32_t);
				(*reinterpret_cast<Dtor**>(a_ptr))[0](a_ptr, 1);
			}
		}

		// nullopt = no answer (no manager, transitional pathing state, off-mesh).
		std::optional<RE::NiPoint3> SnapToNavmesh(RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_offset)
		{
			using GetRefrPathingPosition_t = void* (*)(RE::TESObjectREFR*, PathingPoint*);
			using CreatePathingLocation_t = void (*)(void*, PathingLocation*, const PathingPoint*);
			using FindTriangleForLocation_t = bool (*)(PathingLocation*, TraversableFilter*);
			static const REL::Relocation<std::uintptr_t>             managerGlobal{ REL::ID(937453) };
			static const REL::Relocation<std::uintptr_t>             rebaseTable{ REL::ID(944493) };  // {u32 count; NiPoint3* data}
			static const REL::Relocation<std::uintptr_t>             filterVtable{ REL::ID(478388) };
			static const REL::Relocation<GetRefrPathingPosition_t>   getRefrPathingPosition{ REL::ID(63394) };
			static const REL::Relocation<CreatePathingLocation_t>    createPathingLocation{ REL::ID(71889) };
			static const REL::Relocation<FindTriangleForLocation_t>  findTriangleForLocation{ REL::ID(133636) };

			void* const manager = *reinterpret_cast<void* const*>(managerGlobal.address());
			if (!manager) {
				return std::nullopt;
			}

			PathingPoint point{};
			getRefrPathingPosition(a_ref, &point);
			if (!point.ctx) {
				return std::nullopt;
			}
			// Menu/load transitional states return a (50,50,0) + bogus-cell fallback.
			if (point.pos.GetSquaredDistance(a_ref->data.location) > 1.0f) {
				return std::nullopt;
			}
			point.pos += a_offset;

			PathingLocation loc{};
			createPathingLocation(manager, &loc, &point);

			TraversableFilter filter{};
			filter.vptr = reinterpret_cast<const void*>(filterVtable.address());

			std::optional<RE::NiPoint3> result;
			if (findTriangleForLocation(&loc, &filter)) {
				RE::NiPoint3 pos = loc.pos;
				const auto   count = *reinterpret_cast<const std::uint32_t*>(rebaseTable.address());
				if (loc.spaceIdx < count) {
					if (const auto* data = *reinterpret_cast<RE::NiPoint3* const*>(rebaseTable.address() + 8)) {
						pos += data[loc.spaceIdx];
					}
				}
				result = pos;
			}
			ReleasePathingRef(loc.navmeshRef);
			ReleasePathingRef(loc.spaceObject);
			return result;
		}

		std::optional<float> CurrentViewHeading()
		{
			auto* playerCamera = RE::PlayerCamera::GetSingleton();
			const auto cameraRoot = playerCamera ? playerCamera->cameraRoot : nullptr;
			if (!cameraRoot) {
				return std::nullopt;
			}

			// Row 1 of the rotation is +Y/forward; flatten to the ground plane.
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
		RE::NiPoint3 pos{
			base.pos.x - std::sin(heading) * a_distance,
			base.pos.y + std::cos(heading) * a_distance,
			base.pos.z
		};

		// Lateral correction may span the whole distance (facing a wall); a Z jump past the
		// window means another storey answered, so keep the unsnapped point.
		const RE::NiPoint3 offset{ pos.x - base.pos.x, pos.y - base.pos.y, 0.0f };
		bool snapped = false;
		if (const auto corrected = SnapToNavmesh(a_ref, offset)) {
			constexpr float kZWindow = 2.0f;  // meters
			const float     dx = corrected->x - pos.x;
			const float     dy = corrected->y - pos.y;
			const float     lateralMax = a_distance + 1.0f;
			if (std::abs(corrected->z - pos.z) <= kZWindow && dx * dx + dy * dy <= lateralMax * lateralMax) {
				pos = *corrected;
				snapped = true;
			}
		}

		REX::DEBUG("[Scene] front-of-view anchor ref {:#010x}: origin ({:.1f},{:.1f},{:.1f}) actorHeading {:.2f}, "
			"viewHeading {:.2f}{} distance {:.1f} -> ({:.1f},{:.1f},{:.1f}){}",
			a_ref->GetFormID(), base.pos.x, base.pos.y, base.pos.z, base.heading, heading,
			viewHeading ? "" : " (actor fallback)", a_distance, pos.x, pos.y, pos.z,
			snapped ? " (navmesh-snapped)" : " (no snap)");
		return SceneRuntime::AnchorOverride{ true, pos, heading };
	}

	std::optional<SceneRuntime::AnchorOverride> ResolveSceneAnchor(
		std::string_view a_sceneId, RE::TESObjectREFR* a_ref, std::optional<float> a_headingRad, bool a_emitHud)
	{
		const auto def = Registry::ContentRegistry::GetSingleton().Find(a_sceneId);
		if (!def || !def->RequiresAnchor()) {
			return MakeAnchorAt(a_ref, a_headingRad);
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
		const RefTransform base = RenderedTransform(a_ref);
		const float        baseHeading = a_headingRad ? *a_headingRad : base.heading;
		return ComposeAnchor(base.pos, baseHeading, def->anchorOffset);
	}
}
