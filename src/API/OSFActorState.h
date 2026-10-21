#pragma once
#include <cstdint>

namespace RE { class Actor; class TESBoundObject; }
namespace OSF::API
{
	enum class ActorStateLifetime : std::uint32_t { Persistent, Session };
	enum class ActorEquipmentOperation : std::uint32_t { Equip, Preserve, Hide, HideOptional };
	enum class ActorStateStatus : std::uint32_t { Ok, Invalid, Missing, Conflict, Unavailable, Failed };
	enum ActorEquipmentFlags : std::uint32_t {
		ActorEquipmentAllowAdd = 1,        // Equip only; add one transient copy only if absent
		ActorEquipmentReplaceGroup = 2     // Equip only; replace conflicting equip claims in this scope/group
	};
	// Read-only base-form observation; inventory state is not mesh readiness.
	struct ActorEquipmentState {
		std::uint32_t size{sizeof(ActorEquipmentState)}, count{};
		std::uint64_t slots{}, conflictingWornSlots{};
		std::uint32_t worn{};
	};
	// Runtime-only revisions. Equipment includes observed inventory/availability changes and
	// attempted H2 writes, even on failure; it is not an operation count or a mesh-ready flag.
	struct ActorStateRevision {
		std::uint32_t size{sizeof(ActorStateRevision)};
		std::uint64_t world{}, ownership{}, equipment{};
	};
	enum class ActorStateChangeCause : std::uint32_t { Ownership, Equipment, Reset };
	struct ActorStateChange {
		std::uint64_t sequence{};
		std::uint32_t actor{}; // zero for the reset barrier
		ActorStateChangeCause cause{};
		ActorStateRevision revision;
	};
	inline constexpr std::uint32_t kActorStateChangeLimit = 256;
	struct ActorStateChanges {
		std::uint32_t size{sizeof(ActorStateChanges)}, count{}, resnapshot{};
		std::uint64_t world{}, cursor{};
		ActorStateChange events[kActorStateChangeLimit]{};
	};
	struct ActorEquipmentRequest {
		std::uint32_t size{sizeof(ActorEquipmentRequest)};
		RE::Actor* actor{};
		RE::TESBoundObject* object{};
		ActorEquipmentOperation operation{ActorEquipmentOperation::Equip};
		std::uint32_t group{};             // caller-owned release lane within this scope
		std::uint32_t flags{};
	};
}
