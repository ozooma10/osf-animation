#include "API/BrowserClipLibrary.h"
#include "API/EngineClient.h"
#include "Util/StringUtil.h"
#include <algorithm>
#include <filesystem>
#include <format>
#include <map>
#include <set>
#include <tuple>

namespace OSF::API::BrowserClipLibrary
{
	std::vector<ClipScene> Build(const Catalog::Snapshot& source)
	{
		std::map<std::string, const Catalog::Scene*> scenes;
		std::set<std::string> reserved, curated;
		for (const auto& scene : source.scenes) {
			scenes.emplace(Util::ToLower(scene.id), &scene);
			reserved.insert(Util::ToLower(scene.id));
		}
		const auto key = [](const Catalog::Scene& scene, const Catalog::SourceClip& clip) {
			const auto group = scene.pack.empty() ? "file:" + Util::ToLower(scene.sourceFile) : "pack:" + Util::ToLower(scene.pack);
			return group + '\n' + Util::ToLower(clip.display) + '\n' + clip.animId;
		};
		std::vector<const Catalog::SourceClip*> clips;
		for (const auto& clip : source.sourceClips) {
			const auto found = scenes.find(Util::ToLower(clip.source));
			if (found == scenes.end()) continue;
			const auto& scene = *found->second;
			if (scene.curatedClip) curated.insert(key(scene, clip));
			if (!clip.available || scene.library || !scene.provider.empty() ||
				std::ranges::any_of(scene.tags, [](const auto& tag) { return Util::ToLower(tag).starts_with("player.emote."); })) continue;
			clips.push_back(&clip);
		}
		// Match the legacy source ordering, key, and FNV-1a ID so existing wheel pins survive.
		std::ranges::sort(clips, [&](const auto* a, const auto* b) {
			return std::tuple{Util::ToLower(scenes.at(Util::ToLower(a->source))->sourceFile), Util::ToLower(a->source), a->node, a->stage, a->clip} <
				std::tuple{Util::ToLower(scenes.at(Util::ToLower(b->source))->sourceFile), Util::ToLower(b->source), b->node, b->stage, b->clip};
		});
		std::map<std::string, const Catalog::SourceClip*> unique;
		for (const auto* clip : clips) {
			const auto identity = key(*scenes.at(Util::ToLower(clip->source)), *clip);
			if (!curated.contains(identity)) unique.try_emplace(identity, clip);
		}
		std::vector<ClipScene> result;
		for (const auto& [identity, clip] : unique) {
			std::uint64_t hash = 14695981039346656037ull;
			for (const unsigned char ch : identity) { hash ^= ch; hash *= 1099511628211ull; }
			std::string id = std::format("osf.scene-clip/{:016x}", hash);
			for (std::uint32_t collision = 1; reserved.contains(Util::ToLower(id)); ++collision)
				id = std::format("osf.scene-clip/{:016x}-{}", hash, collision);
			reserved.insert(Util::ToLower(id));
			auto name = std::filesystem::path{clip->display}.filename().string();
			if (name.empty()) name = clip->display;
			if (!clip->animId.empty()) name += " · " + clip->animId;
			ClipScene entry;
			entry.id = std::move(id); entry.source = clip->source; entry.name = std::move(name);
			entry.node = clip->node; entry.stage = clip->stage; entry.clip = clip->clip;
			entry.tags = {"scene.clip"};
			entry.unlisted = entry.library = entry.followActor = entry.clearOffset = true;
			entry.playerInputLock = entry.hideApparel = entry.fade = false;
			result.push_back(std::move(entry));
		}
		return result;
	}

	void Decorate(Catalog::Snapshot& catalog)
	{
		for (auto& scene : catalog.scenes) {
			if (scene.provider != kOwner) continue;
			scene.sourceKind = "derivedDebugAnimation";
			for (auto& file : catalog.files) if (file.path == scene.sourcePath) { ++file.clipEntries; break; }
		}
	}

	bool Install()
	{
		return EngineClient::Get().RegisterClipSceneProvider(kOwner,
			[](const char* text, TextCallback emit, void* output, void*) noexcept {
				try {
					const auto source = nlohmann::json::parse(text).get<Catalog::Snapshot>();
					const auto result = nlohmann::json(Build(source)).dump();
					emit(result.c_str(), output);
				} catch (...) { REX::ERROR("[Registry] Could not build browser clip library"); }
			}, nullptr);
	}
}
