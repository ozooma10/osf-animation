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
	std::vector<ClipScene> Build(const Catalog::ClipSourceSnapshot& source)
	{
		struct Source {
			const Catalog::ClipSourceNode* scene;
			std::string file, group;
			bool excluded;
		};
		std::map<std::string, Source> scenes;
		std::set<std::string> reserved, curated;
		for (const auto& scene : source.nodes) {
			auto id = Util::ToLower(scene.id);
			auto file = Util::ToLower(scene.sourceFile);
			auto group = scene.pack.empty() ? "file:" + file : "pack:" + Util::ToLower(scene.pack);
			const bool excluded = scene.library || !scene.provider.empty() ||
				std::ranges::any_of(scene.tags, [](const auto& tag) { return Util::ToLower(tag).starts_with("player.emote."); });
			scenes.emplace(id, Source{&scene, std::move(file), std::move(group), excluded});
			reserved.insert(std::move(id));
		}
		struct Candidate {
			const Catalog::SourceClip* clip;
			const std::string* source;
			const Source* info;
			auto Order() const { return std::tie(info->file, *source, clip->stage, clip->clip); }
		};
		// Select the same minimum source address directly for each identity. There is no need to
		// sort every occurrence or normalize/resolve scene metadata inside a sort comparator.
		std::map<std::string, Candidate> unique;
		for (const auto& clip : source.sourceClips) {
			const auto found = scenes.find(Util::ToLower(clip.source));
			if (found == scenes.end()) continue;
			const auto& info = found->second;
			if (!info.scene->curatedClip && (!clip.available || info.excluded)) continue;
			auto identity = info.group + '\n' + Util::ToLower(clip.display) + '\n' + clip.animId;
			if (info.scene->curatedClip) curated.insert(identity);
			if (!clip.available || info.excluded) continue;
			const Candidate candidate{&clip, &found->first, &info};
			auto [it, inserted] = unique.try_emplace(std::move(identity), candidate);
			if (!inserted && candidate.Order() < it->second.Order()) it->second = candidate;
		}
		std::vector<ClipScene> result;
		result.reserve(unique.size());
		// Identity order, curated precedence and FNV-1a collision suffixes preserve existing pins.
		for (const auto& [identity, candidate] : unique) {
			if (curated.contains(identity)) continue;
			const auto* clip = candidate.clip;
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
			entry.stage = clip->stage; entry.clip = clip->clip;
			entry.tags = {"scene.clip"};
			entry.unlisted = entry.library = entry.followActor = entry.clearOffset = true;
			entry.playerInputLock = entry.hideApparel = entry.fade = false;
			result.push_back(std::move(entry));
		}
		return result;
	}

	std::vector<ClipScene> BuildFromCatalogJson(const char* text)
	{
		const auto source = nlohmann::json::parse(text).get<Catalog::ClipSourceSnapshot>();
		if (source.schema != 2) throw std::runtime_error("unsupported clip-source catalog schema");
		return Build(source);
	}
	void Decorate(Catalog::Snapshot& catalog)
	{
		for (auto& scene : catalog.nodes) {
			if (scene.provider != kOwner) continue;
			scene.sourceKind = "derivedDebugAnimation";
			for (auto& file : catalog.files) if (file.path == scene.sourcePath) { ++file.clipEntries; break; }
		}
	}

	bool Install()
	{
		return EngineClient::Get().RegisterClipSourceProvider(kOwner,
			[](const char* text, TextCallback emit, void* output, void*) noexcept {
				try {
					const auto result = nlohmann::json(BuildFromCatalogJson(text)).dump();
					emit(result.c_str(), output);
				} catch (...) { REX::ERROR("[Registry] Could not build browser clip library"); }
			}, nullptr);
	}
}
