#pragma once
#include "ActorState/Store.h"

namespace OSF::Equipment
{
	// Game-context inventory backend. Ownership and reconciliation live in ActorState::Store.
	// Base forms are the supported equipment granularity; H4's exact resource instances are separate.
	class EquipmentService final : public ActorState::Backend
	{
	public:
		static EquipmentService& GetSingleton();
		std::optional<std::vector<ActorState::Item>> Read(ActorState::Actor actor) override;
		std::optional<ActorState::Item> Describe(ActorState::Object object) override;
		bool SetWorn(ActorState::Actor actor, ActorState::Object object, bool worn) override;
		bool Add(ActorState::Actor actor, ActorState::Object object) override;
		bool Remove(ActorState::Actor actor, ActorState::Object object) override;
	};
}
