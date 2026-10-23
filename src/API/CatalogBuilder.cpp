#include "API/CatalogBuilder.h"
#include "Profiling/Profile.h"
#include "Registry/ContentRegistry.h"
#include "Serialization/ClipDurations.h"
#include "Scene/SceneTiming.h"
#include "Util/ClipPath.h"
#include "Util/Species.h"
#include "Util/StringUtil.h"
#include <unordered_set>

namespace OSF::API::CatalogBuilder
{
	namespace
	{
		Catalog::SourceClip BuildSourceClip(const Registry::ContentRegistrySnapshot& registry,
			const Registry::NodeDef& def, std::uint32_t s, std::uint32_t c)
		{
			const auto& clip = def.stages[s].clips[c];
			const auto available = registry.clipAvailability.find(clip.file);
			return {def.id, clip.file, clip.animId, Util::ClipSpecDisplay(std::filesystem::path{clip.file}),
				Util::SpeciesFromAnimPath(clip.file), s, c, clip.sec,
				available != registry.clipAvailability.end() && available->second};
		}

		Catalog::Stage BuildStage(const Registry::NodeDef& a_def, std::size_t a_index)
		{
			Catalog::Stage stage;
			stage.index = static_cast<std::int32_t>(a_index);
			const auto& source = a_def.stages[a_index];
			stage.resolved = true;
			stage.name = source.name;
			stage.tags = source.tags;
			stage.hold = source.hold;
			stage.clipCount = static_cast<std::int32_t>(source.clips.size());
			stage.timerSec = source.timer;
			stage.loopCount = source.loops;
			if (a_index + 1 == a_def.stages.size()) Scene::ApplyTerminalTiming(stage.loopCount, stage.timerSec, a_def);
			stage.loopMode = stage.loopCount > 0 ? "count" : "hold";
			if (!source.clips.empty()) {
				const auto& first = source.clips.front();
				stage.loopSec = first.sec > 0 ? first.sec :
					Serialization::ClipDurations::Lookup(first.file, first.animId).value_or(-1.0f);
				if (first.sec <= 0 && !first.file.empty()) stage.durationKey = Serialization::ClipDurations::KeyFor(first.file, first.animId);
			}
			return stage;
		}

		Catalog::Node BuildNode(const Registry::NodeDef& a_def, const std::filesystem::path& a_contentRoot)
		{
			Catalog::Node scene;
			scene.id = a_def.id;
			scene.provider = a_def.provider;
			scene.name = a_def.name;
			scene.species = a_def.species;
			scene.pack = a_def.pack;
			scene.folder = a_def.folder;
			const auto name = a_def.sourceFile.filename().u8string();
			scene.sourceFile.assign(name.begin(), name.end());
			scene.sourcePath = scene.sourceFile;
			if (!a_contentRoot.empty()) {
				const auto relative = a_def.sourceFile.lexically_relative(a_contentRoot).generic_string();
				if (!relative.empty() && relative != ".." && !relative.starts_with("../")) scene.sourcePath = relative;
			}
			scene.sourceKind = Registry::CatalogSourceKindName(a_def.sourceKind);
			scene.placement = a_def.worldPlacement == Animation::WorldPlacementMode::kFollowActor ? "followActor" : "anchorAndPin";
			scene.priority = a_def.priority;
			scene.weight = a_def.weight;
			scene.actorCount = ActorCount(a_def);
			scene.unlisted = a_def.unlisted;
			scene.curatedClip = a_def.curatedClip;
			scene.library = a_def.library;
			scene.clipsAvailable = a_def.clipsAvailable;
			scene.playerInputLock = a_def.playerInputLock;
			scene.hideApparel = a_def.hideApparel;
			scene.fade = a_def.fade;
			scene.tags = a_def.tags;
			scene.anchorKeywords = a_def.anchorKeywords;
			scene.anchorBaseForms = a_def.anchorBaseForms;
			for (const auto& role : a_def.roles) {
				scene.roles.push_back({ role.name, role.gender == Registry::RoleGender::kMale ? "male" :
					role.gender == Registry::RoleGender::kFemale ? "female" : "any" });
			}
			for (std::size_t i = 0; i < a_def.stages.size(); ++i) scene.stages.push_back(BuildStage(a_def, i));
			return scene;
		}

		Catalog::File BuildFile(const Registry::ContentFileStats& a_file)
		{
			Catalog::File file;
			file.file = a_file.file;
			file.path = a_file.path;
			file.pack = a_file.pack;
			file.library = a_file.library;
			file.schema = a_file.schema;
			file.bytes = a_file.bytes;
			file.parseMs = a_file.parseMs;
			file.declaredNodes = a_file.declaredNodes;
			file.hidden = a_file.hidden;
			file.rejectedNodes = a_file.rejectedNodes;
			file.unlisted = a_file.unlisted;
			file.anchored = a_file.anchored;
			file.nodes = a_file.nodes;
			file.stages = a_file.stages;
			file.roles = a_file.roles;
			file.clips = a_file.clips;
			file.distinctClips = a_file.distinctClips;
			file.missingClips = a_file.missingClips;
			file.cues = a_file.cues;
			file.actions = a_file.actions;
			file.sounds = a_file.sounds;
			file.cameras = a_file.cameras;
			file.clipEntries = a_file.clipEntries;
			file.errors = a_file.errors;
			file.warnings = a_file.warnings;
			file.species = a_file.species;
			file.missingClipExamples = a_file.missingClipExamples;
			for (const auto& problem : a_file.problems) {
				file.problems.push_back({ problem.warning, problem.code, problem.message, problem.hint,
					problem.node, problem.role, problem.clip });
			}
			return file;
		}
	}

	std::uint32_t ActorCount(const Registry::NodeDef& a_def)
	{
		if (!a_def.roles.empty()) return static_cast<std::uint32_t>(a_def.roles.size());
		return !a_def.stages.empty() ? static_cast<std::uint32_t>(a_def.stages.front().clips.size()) : 0;
	}

	Catalog::Snapshot Build(const Registry::ContentRegistrySnapshot& a_registry,
		const std::filesystem::path& a_contentRoot)
	{
		OSF_PROFILE_SCOPE("API.CatalogBuild");
		Catalog::Snapshot out;
		out.nodes.reserve(a_registry.nodes.size());
		std::unordered_set<std::string> seen;
		for (const auto& [id, def] : a_registry.nodes) {
			out.nodes.push_back(BuildNode(def, a_contentRoot));
			for (std::uint32_t s = 0; s < def.stages.size(); ++s)
				for (std::uint32_t c = 0; c < def.stages[s].clips.size(); ++c) {
					const auto& clip = def.stages[s].clips[c];
					out.sourceClips.push_back(BuildSourceClip(a_registry, def, s, c));
					if (def.clipsAvailable && !clip.file.empty() && clip.sec <= 0) {
						auto key = Serialization::ClipDurations::KeyFor(clip.file, clip.animId);
						if (seen.insert(key).second) out.clips.push_back({std::move(key), clip.file, clip.animId});
					}
				}
		}
		out.files.reserve(a_registry.files.size());
		for (const auto& file : a_registry.files) out.files.push_back(BuildFile(file));
		out.errors = a_registry.loadErrors;
		return out;
	}

	Catalog::ClipSourceSnapshot BuildClipSources(const Registry::ContentRegistrySnapshot& registry)
	{
		OSF_PROFILE_SCOPE("API.ClipSourceBuild");
		Catalog::ClipSourceSnapshot out;
		out.nodes.reserve(registry.nodes.size());
		for (const auto& [id, def] : registry.nodes) {
			const auto filename = def.sourceFile.filename().u8string();
			out.nodes.push_back({def.id, std::string(filename.begin(), filename.end()), def.pack,
				def.provider, def.tags, def.library, def.curatedClip});
			for (std::uint32_t s = 0; s < def.stages.size(); ++s)
				for (std::uint32_t c = 0; c < def.stages[s].clips.size(); ++c)
					out.sourceClips.push_back(BuildSourceClip(registry, def, s, c));
		}
		return out;
	}
}
