#include "Registry/ContentRegistry.h"
#include "Profiling/Profile.h"
#include "Registry/ContentRegistryClips.h"
#include "Util/StringUtil.h"
#include "Util/Species.h"

namespace OSF::Registry
{
	bool ContentRegistry::RegisterClipProvider(std::string owner, ClipProvider provider)
	{
		if (owner.empty() || owner.size() > 128 || !provider) return false;
		std::lock_guard lock(providerMutex);
		const auto [it, inserted] = providers.emplace(std::move(owner), std::move(provider));
		if (!inserted) return false;
		try { PublishProviders(); }
		catch (...) { providers.erase(it); throw; }
		return true;
	}

	bool ContentRegistry::UnregisterClipProvider(std::string_view owner)
	{
		std::lock_guard lock(providerMutex);
		auto removed = providers.extract(std::string(owner));
		if (removed.empty()) return false;
		try { PublishProviders(); }
		catch (...) { providers.insert(std::move(removed)); throw; }
		return true;
	}

	void ContentRegistry::PublishProviders()
	{
		OSF_PROFILE_SCOPE("Registry.PublishProviders");
		auto next = std::make_shared<ContentRegistrySnapshot>(*baseSnapshot);
		for (const auto& [owner, provider] : providers) {
			try {
				const auto entries = provider(*baseSnapshot);
				if (entries.size() > 100000) throw std::runtime_error("too many clip scenes");
				std::unordered_map<std::string, NodeDef> additions;
				for (const auto& entry : entries) {
					const auto id = Util::ToLower(entry.id);
					if (id.empty() || id.size() > 512 || next->nodes.contains(id) || additions.contains(id))
						throw std::runtime_error("invalid or duplicate scene id");
					const auto source = baseSnapshot->nodes.find(Util::ToLower(entry.source));
					if (source == baseSnapshot->nodes.end())
						throw std::runtime_error("unknown source node");
					const auto& node = source->second;
					if (entry.stage >= node.stages.size() || entry.clip >= node.stages[entry.stage].clips.size())
						throw std::runtime_error("unknown source stage or clip");
					auto clip = node.stages[entry.stage].clips[entry.clip];
					const auto installed = baseSnapshot->clipAvailability.find(clip.file);
					if (installed == baseSnapshot->clipAvailability.end() || !installed->second)
						throw std::runtime_error("source clip is unavailable");
					if (entry.clearOffset) clip.offset.reset();
					NodeDef def;
					def.id = entry.id;
					def.name = entry.name;
					def.provider = owner;
					def.sourceKind = CatalogSourceKind::kProvidedScene;
					def.sourceFile = source->second.sourceFile;
					def.pack = source->second.pack;
					def.folder = source->second.folder;
					def.species = Util::SpeciesFromAnimPath(clip.file);
					if (def.species.empty()) def.species = "human";
					def.unlisted = entry.unlisted;
					def.library = entry.library;
					def.playerInputLock = entry.playerInputLock;
					def.hideApparel = entry.hideApparel;
					def.fade = entry.fade;
					def.worldPlacement = Animation::WorldPlacementFromFollowActor(entry.followActor);
					def.roles.emplace_back();
					def.tags = entry.tags;
					for (const auto& tag : def.tags) def.tagSet.insert(Util::ToLower(tag));
					StageDef stage;
					stage.name = def.name;
					stage.tags = def.tags;
					stage.clips.push_back(std::move(clip));
					def.stages.push_back(std::move(stage));
					additions.emplace(id, std::move(def));
				}
				next->nodes.merge(additions);
			} catch (const std::exception& e) {
				REX::ERROR("[Registry] content provider '{}' rejected: {}", owner, e.what());
			} catch (...) {
				REX::ERROR("[Registry] content provider '{}' failed", owner);
			}
		}
		FinalizePublication(*next);
		snapshot.store(std::move(next), std::memory_order_release);
	}
}
