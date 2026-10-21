#pragma once
#include "API/OSFActorState.h"

namespace OSF::API::ActorStateAPI
{
	std::uint64_t CreateScope(const char*, ActorStateLifetime, std::int32_t);
	ActorStateStatus AcquireEquipment(std::uint64_t, const ActorEquipmentRequest*, std::uint64_t*);
	ActorStateStatus ReleaseClaim(std::uint64_t, std::uint64_t);
	ActorStateStatus ReleaseGroup(std::uint64_t, std::uint32_t);
	ActorStateStatus CloseScope(std::uint64_t);
	ActorStateStatus Reconcile(RE::Actor*);
	ActorStateStatus ReadEquipment(RE::Actor*, RE::TESBoundObject*, ActorEquipmentState*);
}
