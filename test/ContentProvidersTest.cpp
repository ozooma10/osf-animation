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
		Check(!source.nodes.contains("provider.test"), "providers receive authored content, never their own previous output");
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
		const auto plan = registry.BuildNodePlan(first, 1);
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
	Check(!registry.Find("provider.test") && first && registry.BuildNodePlan(first, 1).has_value(),
		"removal retires future lookup while a running scene can still use its pinned definition");
	Check(!registry.UnregisterClipProvider("test.provider"), "unknown owner removal is refused");
	Check(registry.RegisterClipProvider("bad-reference", [](const auto&) {
		API::ClipScene entry; entry.id = "provider.invalid-index"; entry.source = "test.bare"; entry.clip = 99;
		return std::vector{entry};
	}) && !registry.Find("provider.invalid-index"), "invalid clip indexes are rejected without dereferencing outside source data");
	Check(registry.UnregisterClipProvider("bad-reference"), "invalid provider can be removed");

	// A complete publication may include destinations supplied after file parsing.
	struct RestoreDirectory {
		std::filesystem::path previous = std::filesystem::current_path();
		~RestoreDirectory() { std::filesystem::current_path(previous); }
	} restore;
	std::filesystem::current_path(restore.previous.parent_path() / "provider-routes");
	registry.LoadAll();
	Check(registry.Size() == 2 && registry.Find("route/generated") && !registry.Find("route/provider") &&
		registry.LoadErrors().size() == 2,
		"generated clip destinations resolve while absent provider routes and dependents are rejected");
	const auto absent = registry.LoadErrors();
	const auto routeProvider = [](const auto&) {
		API::ClipScene entry; entry.id = "provider/destination"; entry.source = "route/source";
		return std::vector{entry};
	};
	Check(registry.RegisterClipProvider("routes", routeProvider), "route provider registers");
	const auto pinned = registry.Find("route/dependent");
	const auto files = registry.FileStats();
	Check(pinned && registry.Find("route/provider", pinned) && registry.Find("provider/destination", pinned) &&
		registry.Size() == 4 && registry.LoadErrors().empty(),
		"provider publication restores global routes and their dependents without reload");
	Check(files.size() == 1 && files[0].nodes == 4 && files[0].rejectedNodes == 0 &&
		files[0].clipEntries == 1 && files[0].errors == 0 && files[0].problems.empty(),
		"publication statistics exclude generated/provider nodes and retire resolved route errors");
	Check(registry.UnregisterClipProvider("routes") && !registry.Find("route/dependent") &&
		registry.LoadErrors() == absent && registry.Size() == 2,
		"provider removal revalidates routes and reports deterministic diagnostics");
	Check(registry.Find("provider/destination", pinned) && registry.Find("route/provider", pinned),
		"an existing route retains its complete original publication after provider removal");
	const auto removed = registry.FileStats();
	Check(removed.size() == 1 && removed[0].nodes == 2 && removed[0].rejectedNodes == 2 &&
		removed[0].errors == 2 && removed[0].problems.size() == 2,
		"removal recomputes accepted/rejected counts without accumulating diagnostics");
	Check(registry.RegisterClipProvider("routes", routeProvider) && registry.Find("route/dependent") &&
		registry.LoadErrors().empty(), "provider re-registration restores previously rejected routes");
	Check(registry.UnregisterClipProvider("routes"), "route provider fixture releases its registration");
}
