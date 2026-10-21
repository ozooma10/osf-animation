#include "API/ActorStateAPI.h"
#include "ActorState/Service.h"
#include "Scene/SceneRuntime.h"

namespace OSF::API::ActorStateAPI
{
	namespace {
		ActorStateStatus Result(ActorState::Status status)
		{
			ActorState::Report(status, "native request");
			return static_cast<ActorStateStatus>(status);
		}
		static_assert(static_cast<unsigned>(ActorState::Status::Failed) == static_cast<unsigned>(ActorStateStatus::Failed));
		static_assert(static_cast<unsigned>(ActorState::Operation::HideOptional) == static_cast<unsigned>(ActorEquipmentOperation::HideOptional));
	}
	std::uint64_t CreateScope(const char* owner, ActorStateLifetime lifetime, std::int32_t scene)
	{
		try {
			if (!owner) return 0;
			if (lifetime == ActorStateLifetime::Session && Scene::SceneRuntime::GetSingleton().GetNode(scene).empty()) return 0;
			return ActorState::Get().CreateScope(owner, static_cast<ActorState::Lifetime>(lifetime), static_cast<std::uint32_t>(scene));
		} catch (...) { return 0; }
	}
	ActorStateStatus AcquireEquipment(std::uint64_t scope, const ActorEquipmentRequest* request, std::uint64_t* claim)
	{
		if (claim) *claim = 0;
		if (!request || !claim || request->size < sizeof(ActorEquipmentRequest) || !request->actor || !request->object ||
			request->operation > ActorEquipmentOperation::HideOptional || (request->flags & ~3u) ||
			(request->flags && request->operation != ActorEquipmentOperation::Equip)) return ActorStateStatus::Invalid;
		try {
			const auto session = ActorState::Get().SessionOf(scope);
			if (!session || (*session && static_cast<std::uint32_t>(Scene::SceneRuntime::GetSingleton().GetSceneForActor(request->actor)) != *session))
				return ActorStateStatus::Invalid;
			const auto result = ActorState::Acquire(scope, request->actor, request->object,
				static_cast<ActorState::Operation>(request->operation), request->group,
				(request->flags & ActorEquipmentAllowAdd) != 0, (request->flags & ActorEquipmentReplaceGroup) != 0);
			*claim = result.handle;
			return static_cast<ActorStateStatus>(result.status);
		} catch (...) { return ActorStateStatus::Failed; }
	}
	ActorStateStatus ReleaseClaim(std::uint64_t scope, std::uint64_t claim)
	{
		try { return Result(ActorState::Get().Release(scope, claim)); } catch (...) { return ActorStateStatus::Failed; }
	}
	ActorStateStatus ReleaseGroup(std::uint64_t scope, std::uint32_t group)
	{
		try { return Result(ActorState::Get().ReleaseGroup(scope, group)); } catch (...) { return ActorStateStatus::Failed; }
	}
	ActorStateStatus CloseScope(std::uint64_t scope)
	{
		try { return Result(ActorState::Get().CloseScope(scope)); } catch (...) { return ActorStateStatus::Failed; }
	}
	ActorStateStatus Reconcile(RE::Actor* actor)
	{
		if (!actor) return ActorStateStatus::Invalid;
		try { return Result(ActorState::Get().Reconcile(actor->GetFormID())); } catch (...) { return ActorStateStatus::Failed; }
	}
}
