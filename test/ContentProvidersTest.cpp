#include "Check.h"
#include "Registry/ContentRegistry.h"

void CheckContentProviders()
{
	using OSF::Test::Check;
	using namespace OSF;
	auto& registry = Registry::ContentRegistry::GetSingleton();
	registry.LoadAll();
	const auto count = registry.Size();
	int calls = 0;
	const auto provider = [&](const Registry::ContentRegistrySnapshot& source) {
		++calls;
		Check(!source.scenes.contains("provider.test"), "providers receive authored content, never their own previous output");
		API::ClipScene scene;
		scene.id = "provider.test"; scene.source = "test.bare"; scene.name = "Provided clip";
		scene.unlisted = scene.library = scene.followActor = scene.clearOffset = true;
		scene.playerInputLock = scene.hideApparel = false;
		return std::vector{scene};
	};
	Check(registry.RegisterClipProvider("test.provider", provider), "provider registers after the base catalog loads");
	const auto first = registry.Find("provider.test");
	Check(first && first->provider == "test.provider" && first->unlisted && !first->playerInputLock && !first->hideApparel &&
		first->worldPlacement == Animation::WorldPlacementMode::kFollowActor && registry.Size() == count,
		"provided clip scenes resolve by ID with caller-selected policy without inflating authored counts");
	if (first) {
		const auto plan = registry.BuildNodePlan(first, first->nodes.front(), 1);
		Check(plan.has_value(), "provided scene builds through the ordinary runtime playback-plan path");
	}
	Check(!registry.RegisterClipProvider("test.provider", provider), "duplicate owner registration is refused");
	registry.LoadAll();
	Check(calls == 2 && registry.Find("provider.test") && first && first->name == "Provided clip",
		"reload rebuilds providers while existing scene references stay valid");
	Check(registry.RegisterClipProvider("invalid.provider", [](const auto&) {
		API::ClipScene good; good.id = "provider.partial"; good.source = "test.bare";
		auto bad = good; bad.id = "TEST.BARE";
		return std::vector{good, bad};
	}), "a provider may register even when its current output is rejected");
	Check(!registry.Find("provider.partial") && registry.Find("test.bare") && registry.Find("provider.test"),
		"a colliding provider batch is rejected atomically without removing authored or other-provider scenes");
	Check(registry.UnregisterClipProvider("invalid.provider") && registry.UnregisterClipProvider("test.provider"), "providers unregister");
	Check(!registry.Find("provider.test") && first && registry.BuildNodePlan(first, first->nodes.front(), 1).has_value(),
		"removal retires future lookup while a running scene can still use its pinned definition");
	Check(!registry.UnregisterClipProvider("test.provider"), "unknown owner removal is refused");
	Check(registry.RegisterClipProvider("bad-reference", [](const auto&) {
		API::ClipScene entry; entry.id = "provider.invalid-index"; entry.source = "test.bare"; entry.clip = 99;
		return std::vector{entry};
	}) && !registry.Find("provider.invalid-index"), "invalid clip indexes are rejected without dereferencing outside source data");
	Check(registry.UnregisterClipProvider("bad-reference"), "invalid provider can be removed");
}
