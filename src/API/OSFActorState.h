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
	struct ActorEquipmentRequest {
		std::uint32_t size{sizeof(ActorEquipmentRequest)};
		RE::Actor* actor{};
		RE::TESBoundObject* object{};
		ActorEquipmentOperation operation{ActorEquipmentOperation::Equip};
		std::uint32_t group{};             // caller-owned release lane within this scope
		std::uint32_t flags{};
	};
}
