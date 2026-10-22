#include "Registry/ContentRegistryClips.h"

#include "Animation/GraphManager.h"
#include "Util/ClipPath.h"
#include "Util/Species.h"
#include "Util/StringUtil.h"

#include <algorithm>
#include <format>
#include <unordered_set>

namespace OSF::Registry::ContentRegistryClips
{
	using Util::ToLower;

	void ProblemSink::Push(std::string a_line, const std::filesystem::path& a_owner,
		std::string a_code, std::string a_hint, std::string a_node, std::string a_role, std::string a_clip)
	{
		const bool warning = a_line.starts_with("[warn]");
		lines.push_back(std::move(a_line));
		records.push_back(PendingImportProblem{
			a_owner, warning, std::move(a_code), std::move(a_hint),
			std::move(a_node), std::move(a_role), std::move(a_clip)
		});
	}

	// Mounted BSResource (loose or archive) or an existing absolute file outside Data.
	bool ClipSpecInstalled(const std::string& a_spec)
	{
		return Util::ProbeClipSpec(std::filesystem::path{ a_spec },
			[](std::string_view resource) { return Animation::ResourceExists(resource); },
			[](const std::filesystem::path& file) {
				std::error_code error;
				return std::filesystem::exists(file, error);
			});
	}

	namespace
	{
		bool ClipInstalled(ClipInstalledCache& a_cache, const std::string& a_file)
		{
			auto [it, fresh] = a_cache.try_emplace(a_file, false);
			if (fresh) {
				it->second = ClipSpecInstalled(a_file);
			}
			return it->second;
		}
	}

	void SweepClipAvailability(std::unordered_map<std::string, NodeDef>& a_scenes,
		ProblemSink& a_problems, ClipInstalledCache& a_cache)
	{
		struct FileTally
		{
			std::filesystem::path source;
			int                   hidden = 0;
			std::string           firstMissing;
		};

		// Full-path keys and an ordered map keep warning attribution and ordering deterministic.
		std::map<std::string, FileTally> byFile;
		for (auto& [key, definition] : a_scenes) {
			std::string missing;
			// Cache every clip, including clips after the first unavailable one.
			for (const auto& stage : definition.stages)
				for (const auto& clip : stage.clips)
					if (!ClipInstalled(a_cache, clip.file) && missing.empty()) missing = clip.file;

			if (!missing.empty()) {
				definition.clipsAvailable = false;
				auto& tally = byFile[definition.sourceFile.string()];
				tally.source = definition.sourceFile;
				++tally.hidden;
				if (tally.firstMissing.empty()) {
					tally.firstMissing = missing;
				}
			}
		}
		for (const auto& [path, tally] : byFile) {
			const auto file = tally.source.filename().string();
			a_problems.Push("[warn] '" + file + "': " + std::to_string(tally.hidden) +
				" scene(s) hidden — clips not installed (e.g. '" + tally.firstMissing +
				"'); install the animation pack this file references", tally.source,
				"missing-clips", "Install the referenced animation pack or correct the clip path, then reload packs.",
				{}, {}, tally.firstMissing);
			REX::WARN("[Registry] '{}': {} scene(s) hidden — clips not installed (e.g. '{}')",
				file, tally.hidden, tally.firstMissing);
		}
	}

	std::size_t AddRegisteredClipEntries(std::unordered_map<std::string, NodeDef>& a_scenes,
		const std::vector<ClipLibraryRegistration>& a_registrations, ProblemSink& a_problems,
		std::map<std::string, std::uint32_t>& a_addedByFile, ClipInstalledCache& a_cache)
	{
		struct ClipEntry
		{
			std::string              display;
			std::string              name;
			std::string              folder;
			std::vector<std::string> tags;
			StageClip                clip;
			std::filesystem::path    sourceFile;
			std::string              pack;
		};

		const auto groupOf = [](std::string_view a_pack, const std::filesystem::path& a_sourceFile) {
			return !a_pack.empty()
			         ? "pack:" + ToLower(std::string(a_pack))
			         : "file:" + ToLower(a_sourceFile.filename().string());
		};
		const auto clipKey = [&groupOf](std::string_view a_pack, const std::filesystem::path& a_sourceFile,
			std::string_view a_display, std::string_view a_animId) {
			return groupOf(a_pack, a_sourceFile) + '\n' + ToLower(std::string(a_display)) + '\n' + std::string(a_animId);
		};

		std::map<std::string, ClipEntry> unique;
		std::unordered_map<std::string, bool> installed;
		std::unordered_set<std::string> explicitKeys;
		for (const auto& registration : a_registrations) {
			const std::string display = Util::ClipSpecDisplay(std::filesystem::path{ registration.clip.file });
			const std::string key = clipKey(registration.pack, registration.sourceFile, display, registration.clip.animId);
			if (!explicitKeys.insert(key).second) {
				a_problems.Push("[error] duplicate clipLibrary registration for '" + display +
					(registration.clip.animId.empty() ? "" : (":" + registration.clip.animId)) +
					"' in pack/file group '" +
					(registration.pack.empty() ? registration.sourceFile.filename().string() : registration.pack) +
					"' — keeping the first", registration.sourceFile, "duplicate-clip-library",
					"Remove or rename the duplicate clipLibrary registration; OSF keeps the first.",
					{}, {}, registration.clip.file);
				REX::ERROR("[Registry] duplicate clipLibrary registration '{}' in group '{}' — keeping first",
					display, registration.pack.empty() ? registration.sourceFile.filename().string() : registration.pack);
				continue;
			}

			const std::string installKey = ToLower(display);
			// Keyed by display so spellings of one clip probe once; mirrored into a_cache under the raw spec.
			auto [installedIt, fresh] = installed.try_emplace(installKey, false);
			if (fresh) {
				installedIt->second = ClipInstalled(a_cache, registration.clip.file);
			} else {
				a_cache.try_emplace(registration.clip.file, installedIt->second);
			}
			if (!installedIt->second) {
				a_problems.Push("[warn] '" + registration.sourceFile.filename().string() +
					"': clipLibrary entry hidden — clip not installed ('" + registration.clip.file + "')",
					registration.sourceFile, "missing-library-clip",
					"Install the referenced animation pack or correct this clip path, then reload packs.",
					{}, {}, registration.clip.file);
				REX::WARN("[Registry] '{}': clipLibrary entry hidden — clip not installed ('{}')",
					registration.sourceFile.filename().string(), registration.clip.file);
				continue;
			}
			unique.emplace(key, ClipEntry{ display, registration.name, registration.folder,
				registration.tags, registration.clip, registration.sourceFile, registration.pack });
		}

		const auto stableHash = [](std::string_view a_text) {
			std::uint64_t hash = 14695981039346656037ull;
			for (const unsigned char character : a_text) {
				hash ^= character;
				hash *= 1099511628211ull;
			}
			return hash;
		};

		std::size_t added = 0;
		for (auto& [key, entry] : unique) {
			std::string id = std::format("osf.scene-clip/{:016x}", stableHash(key));
			for (std::uint32_t collision = 1; a_scenes.contains(ToLower(id)); ++collision) {
				id = std::format("osf.scene-clip/{:016x}-{}", stableHash(key), collision);
			}

			NodeDef definition;
			definition.id = std::move(id);
			definition.name = entry.name;
			if (definition.name.empty()) {
				const std::filesystem::path displayPath{ entry.display };
				definition.name = displayPath.filename().string();
				if (definition.name.empty()) {
					definition.name = entry.display;
				}
				if (!entry.clip.animId.empty()) {
					definition.name += " · " + entry.clip.animId;
				}
			}
			definition.species = Util::SpeciesFromAnimPath(entry.clip.file);
			if (definition.species.empty()) {
				definition.species = "human";
			}
			definition.unlisted = true;
			definition.library = true;
			definition.curatedClip = true;
			definition.sourceKind = CatalogSourceKind::kCuratedAnimation;
			definition.playerInputLock = false;
			definition.hideApparel = false;
			definition.fade = false;
			definition.worldPlacement = Animation::WorldPlacementMode::kFollowActor;
			definition.tags = { "scene.clip" };
			definition.tagSet = { "scene.clip" };
			definition.roles.emplace_back();
			for (const auto& tag : entry.tags) {
				const auto lower = ToLower(tag);
				if (definition.tagSet.insert(lower).second) {
					definition.tags.push_back(tag);
				}
			}
			definition.sourceFile = std::move(entry.sourceFile);
			definition.pack = std::move(entry.pack);
			definition.folder = std::move(entry.folder);

			StageDef stage;
			stage.name = definition.name;
			stage.tags = definition.tags;
			entry.clip.offset.reset();
			stage.clips.push_back(std::move(entry.clip));
			definition.stages.push_back(std::move(stage));

			const std::string generatedKey = ToLower(definition.id);
			++a_addedByFile[definition.sourceFile.string()];
			a_scenes.emplace(generatedKey, std::move(definition));
			++added;
		}
		return added;
	}

	void AccumulateFileStats(const std::unordered_map<std::string, NodeDef>& a_scenes,
		ClipInstalledCache& a_cache, std::vector<ContentFileStats>& a_files,
		const std::unordered_map<std::string, std::size_t>& a_index)
	{
		std::vector<std::unordered_set<std::string>> clipSets(a_files.size());
		std::vector<std::unordered_set<std::string>> speciesSets(a_files.size());
		for (const auto& [key, definition] : a_scenes) {
			if (definition.sourceKind == CatalogSourceKind::kCuratedAnimation ||
				definition.sourceKind == CatalogSourceKind::kProvidedScene) continue;
			const auto indexIt = a_index.find(definition.sourceFile.string());
			if (indexIt == a_index.end()) {
				continue;
			}
			auto& stats = a_files[indexIt->second];
			stats.hidden += definition.clipsAvailable ? 0u : 1u;
			stats.unlisted += definition.unlisted ? 1u : 0u;
			stats.anchored += definition.RequiresAnchor() ? 1u : 0u;
			++stats.nodes;
			stats.roles += static_cast<std::uint32_t>(definition.roles.size());
			if (!definition.species.empty()) {
				speciesSets[indexIt->second].insert(definition.species);
			}
			stats.cues += static_cast<std::uint32_t>(definition.cues.size());
			stats.actions += static_cast<std::uint32_t>(definition.actions.size());
			stats.sounds += static_cast<std::uint32_t>(definition.sounds.size());
			stats.cameras += static_cast<std::uint32_t>(definition.cameras.size());
			stats.stages += static_cast<std::uint32_t>(definition.stages.size());
			for (const auto& stage : definition.stages) {
				stats.clips += static_cast<std::uint32_t>(stage.clips.size());
				for (const auto& clip : stage.clips) {
					clipSets[indexIt->second].insert(clip.file);
				}
			}
		}
		for (std::size_t i = 0; i < a_files.size(); ++i) {
			auto& stats = a_files[i];
			stats.distinctClips = static_cast<std::uint32_t>(clipSets[i].size());
			std::vector<std::string> missing;
			for (const auto& clip : clipSets[i]) {
				if (!ClipInstalled(a_cache, clip)) {
					missing.push_back(clip);
				}
			}
			std::sort(missing.begin(), missing.end());
			stats.missingClips = static_cast<std::uint32_t>(missing.size());
			constexpr std::size_t kMaxMissingExamples = 8;
			stats.missingClipExamples.assign(missing.begin(),
				missing.begin() + std::min(missing.size(), kMaxMissingExamples));
			stats.species.assign(speciesSets[i].begin(), speciesSets[i].end());
			std::sort(stats.species.begin(), stats.species.end());
		}
	}
}
