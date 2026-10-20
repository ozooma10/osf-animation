#pragma once
#include "ActorState/Store.h"
#include <span>

namespace RE { class Actor; class TESBoundObject; }
namespace OSF::ActorState
{
	enum Group : std::uint32_t { Apparel = 1, Held = 2, Authored = 3, Gear = 4 };
	Store& Get();
	Result Acquire(Handle scope, RE::Actor* actor, RE::TESBoundObject* object,
		Operation operation, std::uint32_t group, bool allowAdd = false, bool replaceGroup = false);
	std::size_t Hide(Handle scope, RE::Actor* actor, std::uint32_t slots, bool held,
		std::span<RE::TESBoundObject* const> keep = {});
	void Report(Status status, std::string_view operation);
}
