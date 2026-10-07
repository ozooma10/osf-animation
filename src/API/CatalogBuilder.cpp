#include "API/CatalogBuilder.h"
#include "Registry/ContentRegistry.h"
#include "Serialization/ClipDurations.h"

namespace OSF::API::CatalogBuilder
{
	namespace
	{
		Catalog::Stage BuildStage(const Registry::SceneDef& a_def, std::size_t a_index)
		{
			Catalog::Stage stage;
			stage.index = static_cast<std::int32_t>(a_index);
			const auto* node = a_def.FindNode(a_def.linearStages[a_index]);
			if (!node || node->stages.empty()) return stage;

			const auto& source = node->stages.front();
			stage.resolved = true;
			stage.name = source.name;
			stage.tags = source.tags;
			stage.hold = source.hold;
			stage.clipCount = static_cast<std::int32_t>(source.clips.size());
			stage.timerSec = node->timerSec;
			stage.loopCount = node->loopCount;
			stage.loopMode = node->loopMode == Registry::LoopMode::kHold ? "hold" :
				node->loopMode == Registry::LoopMode::kCount ? "count" : "once";
			if (!source.clips.empty()) {
				const auto& first = source.clips.front();
				stage.loopSec = first.sec > 0 ? first.sec :
					Serialization::ClipDurations::Lookup(first.file, first.animId).value_or(-1.0f);
			}
			return stage;
		}

		Catalog::Scene BuildScene(const Registry::SceneDef& a_def, const std::filesystem::path& a_contentRoot)
		{
			Catalog::Scene scene;
			scene.id = a_def.id;
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
			for (std::size_t i = 0; i < a_def.linearStages.size(); ++i) scene.stages.push_back(BuildStage(a_def, i));
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
			file.scenes = a_file.scenes;
			file.declaredScenes = a_file.declaredScenes;
			file.hidden = a_file.hidden;
			file.rejectedScenes = a_file.rejectedScenes;
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
					problem.scene, problem.node, problem.role, problem.clip });
			}
			return file;
		}
	}

	std::uint32_t ActorCount(const Registry::SceneDef& a_def)
	{
		if (!a_def.roles.empty()) return static_cast<std::uint32_t>(a_def.roles.size());
		const auto* node = a_def.FindNode(a_def.entry);
		if (!node && !a_def.nodes.empty()) node = &a_def.nodes.front();
		return node && !node->stages.empty() ? static_cast<std::uint32_t>(node->stages.front().clips.size()) : 0;
	}

	Catalog::Snapshot Build(const Registry::ContentRegistrySnapshot& a_registry,
		const std::filesystem::path& a_contentRoot)
	{
		Catalog::Snapshot out;
		out.scenes.reserve(a_registry.scenes.size());
		for (const auto& [id, def] : a_registry.scenes) out.scenes.push_back(BuildScene(def, a_contentRoot));
		out.files.reserve(a_registry.files.size());
		for (const auto& file : a_registry.files) out.files.push_back(BuildFile(file));
		out.errors = a_registry.loadErrors;
		return out;
	}
}
