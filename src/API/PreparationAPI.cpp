#include "API/PreparationAPI.h"
#include "Scene/SceneRuntime.h"
#include "Util/NativeMainThreadQueue.h"

namespace OSF::API::PreparationAPI
{
	namespace
	{
		PreparationContext View(const Preparation::Context& c, std::uint64_t provider)
		{
			PreparationContext view;
			view.attempt = c.attempt;
			view.provider = provider;
			view.scene = c.scene;
			view.kind = static_cast<PreparationKind>(c.kind);
			view.fromScene = c.fromScene.c_str();
			view.fromNode = c.fromNode.c_str();
			view.toScene = c.toScene.c_str();
			view.toNode = c.toNode.c_str();
			view.fromStage = c.fromStage;
			view.toStage = c.toStage;
			view.actors = c.actors.data();
			view.actorCount = static_cast<std::uint32_t>(c.actors.size());
			return view;
		}
	}
	std::uint64_t Register(const ScenePreparer* options)
	{
		if (!options || options->size < sizeof(ScenePreparer) || options->version != kPreparationVersion || !options->prepare || !options->finish || !options->timeoutMs || options->timeoutMs > 5000)
			return 0;
		try {
			const auto            copied = *options;
			const auto            token = std::make_shared<std::uint64_t>();
			Preparation::Provider provider;
			provider.order = copied.order;
			provider.timeoutMs = copied.timeoutMs;
			provider.invoke = [copied, token](const Preparation::Context& c, Preparation::Phase phase) {
				const auto view = View(c, *token);
				return static_cast<Preparation::Decision>(copied.prepare(&view, static_cast<PreparationPhase>(phase), copied.context));
			};
			provider.finish = [copied, token](const Preparation::Context& c, Preparation::Outcome outcome) {
				const auto view = View(c, *token);
				copied.finish(&view, static_cast<PreparationOutcome>(outcome), copied.context);
			};
			*token = Scene::SceneRuntime::GetSingleton().RegisterPreparer(std::move(provider));
			return *token;
		} catch (...) {
			return 0;
		}
	}
	bool Unregister(std::uint64_t provider)
	{
		try {
			return Scene::SceneRuntime::GetSingleton().UnregisterPreparer(provider);
		} catch (...) {
			return false;
		}
	}
	bool Equipment(std::uint64_t attempt, std::uint64_t provider, const ActorEquipmentRequest* request)
	{
		if (!request || request->size < sizeof(ActorEquipmentRequest) || !request->actor || !request->object || request->operation > ActorEquipmentOperation::HideOptional || (request->flags & ~ActorEquipmentAllowAdd) ||
			(request->flags && request->operation != ActorEquipmentOperation::Equip))
			return false;
		try {
			return Scene::SceneRuntime::GetSingleton().PlanEquipment(attempt, provider, { request->actor->GetFormID(), request->object->GetFormID(), static_cast<ActorState::Operation>(request->operation), 0, (request->flags & ActorEquipmentAllowAdd) != 0 });
		} catch (...) {
			return false;
		}
	}
	bool Resource(std::uint64_t attempt, std::uint64_t provider, const char* key)
	{
		try {
			return key && Scene::SceneRuntime::GetSingleton().ReserveResource(attempt, provider, key);
		} catch (...) {
			return false;
		}
	}
	bool Failure(std::uint64_t attempt, std::uint64_t provider, const char* reason)
	{
		try {
			return reason && Scene::SceneRuntime::GetSingleton().PreparationFailure(attempt, provider, reason);
		} catch (...) {
			return false;
		}
	}
	bool Read(std::int32_t scene, TextCallback callback, void* context)
	{
		if (!callback || !Util::NativeMainThreadQueue::IsDraining())
			return false;
		try {
			const auto text = Scene::SceneRuntime::GetSingleton().ReadPreparation(scene);
			if (text.empty())
				return false;
			callback(text.c_str(), context);
			return true;
		} catch (...) {
			return false;
		}
	}
}
