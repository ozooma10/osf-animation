#include "PackRegistry.h"

#include "Util/Math.h"
#include "Util/StringUtil.h"

#include <algorithm>
#include <fstream>
#include <random>

#include <nlohmann/json.hpp>

namespace OSF::Registry
{
	namespace
	{
		using OSF::Util::ToLower;

		Animation::ParticipantPlacement ParseOffset(const nlohmann::json& a_json)
		{
			Animation::ParticipantPlacement p{};
			p.x = a_json.value("x", 0.0f);
			p.y = a_json.value("y", 0.0f);
			p.z = a_json.value("z", 0.0f);
			// authors write degrees; the runtime uses radians
			p.heading = static_cast<float>(a_json.value("heading", 0.0) * Util::kDegToRad);
			return p;
		}

		SlotGender ParseGender(const nlohmann::json& a_actor)
		{
			const auto s = ToLower(a_actor.value("gender", "any"));
			if (s == "male" || s == "m") {
				return SlotGender::kMale;
			}
			if (s == "female" || s == "f") {
				return SlotGender::kFemale;
			}
			return SlotGender::kAny;
		}

		bool HasAllTags(const AnimationDef& a_def, const std::vector<std::string>& a_tagsLower)
		{
			for (const auto& want : a_tagsLower) {
				bool found = false;
				for (const auto& have : a_def.tags) {
					if (ToLower(have) == want) {
						found = true;
						break;
					}
				}
				if (!found) {
					return false;
				}
			}
			return true;
		}

		bool SlotsSatisfied(const std::vector<SlotDef>& a_slots, const std::vector<RE::SEX>& a_genders,
			const std::vector<size_t>& a_perm)
		{
			for (size_t slot = 0; slot < a_slots.size(); slot++) {
				switch (a_slots[slot].gender) {
				case SlotGender::kMale:
					if (a_genders[a_perm[slot]] != RE::SEX::kMale) {
						return false;
					}
					break;
				case SlotGender::kFemale:
					if (a_genders[a_perm[slot]] != RE::SEX::kFemale) {
						return false;
					}
					break;
				default:
					break;  // kAny accepts anyone, including SEX::kNone
				}
			}
			return true;
		}

		AnimationDef ParseAnimation(const nlohmann::json& a_json, const std::string& a_packName)
		{
			AnimationDef def;
			def.id = a_json.at("id").get<std::string>();
			if (def.id.empty()) {
				throw std::runtime_error("empty animation id");
			}
			def.name = a_json.value("name", def.id);
			def.pack = a_packName;

			if (auto it = a_json.find("tags"); it != a_json.end()) {
				for (const auto& tag : *it) {
					def.tags.push_back(tag.get<std::string>());
				}
			}

			// actors[]: stage-invariant per-actor metadata only (no per-stage data).
			// The clips an actor plays live in the top-level stages[].clips[].
			const auto& actors = a_json.at("actors");
			if (!actors.is_array() || actors.empty()) {
				throw std::runtime_error("'actors' must be a non-empty array");
			}
			for (const auto& jActor : actors) {
				// Reject the pre-2.0 actor-major layout with a clear hint: clips
				// moved out of actors[].stages[] into the top-level stages[].clips[].
				if (jActor.contains("stages") || jActor.contains("file")) {
					throw std::runtime_error("'" + def.id + "': actor-major layout removed in schema 2 — move each "
						"actor's clips into the top-level stages[].clips[] (see docs/PACK_SCHEMA.md)");
				}
				SlotDef slot;
				slot.gender = ParseGender(jActor);
				if (auto it = jActor.find("offset"); it != jActor.end()) {
					slot.offset = ParseOffset(*it);
				}
				def.actors.push_back(std::move(slot));
			}
			const size_t actorCount = def.actors.size();

			// stages[]: the timeline. Required and non-empty; every stage carries
			// timing plus one clip per actor (clips[], actor order). The OSF content
			// fields (intensity/peak/cues) are ignored by the core parser.
			const auto stagesIt = a_json.find("stages");
			if (stagesIt == a_json.end() || !stagesIt->is_array() || stagesIt->empty()) {
				throw std::runtime_error("'" + def.id + "': 'stages' must be a non-empty array");
			}
			for (const auto& jStage : *stagesIt) {
				StageDef info;
				info.timer = jStage.value("timer", 0.0f);
				info.loops = jStage.value("loops", 0);
				// clips[]: one per actor, in actor order. A bare string is just the
				// file; an object is { file, offset } where offset overrides the
				// actor's default placement for this stage.
				const auto clipsIt = jStage.find("clips");
				if (clipsIt == jStage.end() || !clipsIt->is_array()) {
					throw std::runtime_error("'" + def.id + "': every stage needs a 'clips' array (one clip per actor)");
				}
				for (const auto& jClip : *clipsIt) {
					StageClip clip;
					if (jClip.is_string()) {
						clip.file = jClip.get<std::string>();
					} else if (jClip.is_object()) {
						clip.file = jClip.at("file").get<std::string>();
						if (auto oit = jClip.find("offset"); oit != jClip.end()) {
							clip.offset = ParseOffset(*oit);
						}
					} else {
						throw std::runtime_error("'" + def.id + "': a clip must be a file string or a { file, offset } object");
					}
					if (clip.file.empty()) {
						throw std::runtime_error("'" + def.id + "': empty clip file");
					}
					info.clips.push_back(std::move(clip));
				}
				if (info.clips.size() != actorCount) {
					throw std::runtime_error("'" + def.id + "': stage has " + std::to_string(info.clips.size()) +
						" clip(s) but the scene has " + std::to_string(actorCount) + " actor(s)");
				}
				def.stages.push_back(std::move(info));
			}

			return def;
		}

		// Mutable accumulators for one LoadAll pass.
		struct LoadState
		{
			std::unordered_map<std::string, AnimationDef> animations;
			size_t packCount = 0;
			size_t duplicateAnims = 0;
		};

		// Every *.json under Data/OSF (recursive) except settings files,
		// sorted by filename so "first loaded wins" is deterministic.
		std::vector<std::filesystem::path> CollectPackFiles(const std::filesystem::path& a_dir)
		{
			namespace fs = std::filesystem;
			std::vector<fs::path> files;
			std::error_code ec;
			for (const auto& entry : fs::recursive_directory_iterator(a_dir, ec)) {
				if (!entry.is_regular_file(ec) || ToLower(entry.path().extension().string()) != ".json") {
					continue;
				}
				// settings files are not animation packs: the bare user-owned
				// "settings.json" and mod-namespaced "*.settings.json" (consumed by
				// other OSF layers, skipped here so they aren't misparsed as packs).
				const auto fileName = ToLower(entry.path().filename().string());
				if (fileName == "settings.json" || fileName.ends_with(".settings.json")) {
					continue;
				}
				files.push_back(entry.path());
			}
			std::sort(files.begin(), files.end(), [](const fs::path& a_lhs, const fs::path& a_rhs) {
				return ToLower(a_lhs.filename().string()) < ToLower(a_rhs.filename().string());
			});
			return files;
		}

		// A normal animation pack: schema-gated, contributes animation definitions.
		// A bad single animation is logged and skipped; only a missing/oversized
		// schema skips the whole file.
		void LoadPackFile(const nlohmann::json& a_json, const std::filesystem::path& a_file, LoadState& a_state)
		{
			const std::string fileName = a_file.filename().string();
			std::int64_t schema = 1;  // absent = v1 (pre-versioning packs)
			if (auto it = a_json.find("schema"); it != a_json.end()) {
				if (!it->is_number_integer()) {
					REX::ERROR("PackRegistry: '{}' has a non-integer 'schema' field — pack skipped", fileName);
					return;
				}
				schema = it->get<std::int64_t>();
			}
			if (schema < 1 || schema > kPackSchemaVersion) {
				REX::ERROR("PackRegistry: '{}' declares schema version {} but this build understands up to {} "
					"— pack skipped (update OSF Animation?)",
					fileName, schema, kPackSchemaVersion);
				return;
			}
			const std::string packName = a_json.value("name", a_file.stem().string());
			for (const auto& jAnim : a_json.at("animations")) {
				try {
					auto def = ParseAnimation(jAnim, packName);
					def.sourceFile = a_file;
					auto key = ToLower(def.id);
					if (auto it = a_state.animations.find(key); it != a_state.animations.end()) {
						a_state.duplicateAnims++;
						REX::ERROR("PackRegistry: duplicate animation id '{}' (case-insensitive) in pack '{}' file '{}'; "
							"already registered by pack '{}' file '{}'. Keeping the first-loaded definition and skipping "
							"the duplicate. Use namespaced ids like 'author.pack.anim'.",
							def.id, packName, fileName, it->second.pack, it->second.sourceFile.filename().string());
						continue;
					}
					a_state.animations[std::move(key)] = std::move(def);
				} catch (const std::exception& e) {
					REX::WARN("PackRegistry: skipping animation in '{}': {}", fileName, e.what());
				}
			}
			a_state.packCount++;
		}
	}

	PackRegistry& PackRegistry::GetSingleton()
	{
		static PackRegistry singleton;
		return singleton;
	}

	void PackRegistry::LoadAll()
	{
		namespace fs = std::filesystem;
		LoadState state;

		const fs::path dir = fs::current_path() / "Data" / "OSF";
		std::error_code ec;
		if (fs::is_directory(dir, ec)) {
			for (const auto& packFile : CollectPackFiles(dir)) {
				const std::string fileName = packFile.filename().string();
				try {
					std::ifstream in(packFile, std::ios::binary);
					// last arg: tolerate // comments in hand-edited packs
					const auto json = nlohmann::json::parse(in, nullptr, true, true);
					const auto lowerName = ToLower(fileName);
					// Reserved OSF Intimacy file types (scene defs, voice sets, dialogue
					// manifests): skipped so they aren't misparsed as core anim packs.
					// See docs/INTIMACY_SEAM.md.
					if (lowerName.ends_with(".scene.json") || lowerName.ends_with(".voice.json") ||
						lowerName.ends_with(".dialogue.json")) {
						REX::INFO("PackRegistry: ignoring OSF Intimacy manifest '{}'", fileName);
					} else {
						LoadPackFile(json, packFile, state);
					}
				} catch (const std::exception& e) {
					REX::ERROR("PackRegistry: failed to parse '{}': {}", fileName, e.what());
				}
			}
		}

		const size_t count = state.animations.size();
		{
			std::unique_lock l{ lock };
			animations = std::move(state.animations);
		}
		REX::INFO("PackRegistry: {} animation(s) loaded from {} pack file(s) in {}",
			count, state.packCount, dir.string());
		if (state.duplicateAnims > 0) {
			REX::ERROR("PackRegistry: skipped {} duplicate animation id(s); animation ids are global, case-insensitive, "
				"and should be namespaced as 'author.pack.anim'",
				state.duplicateAnims);
		}
	}

	std::optional<Animation::ScenePlan> PackRegistry::BuildScenePlan(std::string_view a_id, size_t a_actorCount) const
	{
		std::shared_lock l{ lock };

		auto it = animations.find(ToLower(a_id));
		if (it == animations.end()) {
			REX::WARN("PackRegistry: unknown animation id '{}' ({} registered)", a_id, animations.size());
			return std::nullopt;
		}
		const auto& def = it->second;

		if (def.actors.size() != a_actorCount) {
			REX::WARN("PackRegistry: '{}' needs {} actor(s), got {}", def.id, def.actors.size(), a_actorCount);
			return std::nullopt;
		}

		Animation::ScenePlan plan;
		plan.animId = def.id;
		plan.stages.reserve(def.stages.size());
		for (const auto& sd : def.stages) {
			Animation::ScenePlan::Stage stage;
			stage.timer = sd.timer;
			stage.loops = sd.loops;
			for (size_t a = 0; a < def.actors.size(); a++) {
				const auto& clip = sd.clips[a];
				stage.files.push_back(clip.file);
				stage.placements.push_back(clip.offset.value_or(def.actors[a].offset));
			}
			plan.stages.push_back(std::move(stage));
		}
		return plan;
	}

	std::vector<std::string> PackRegistry::FindByTags(size_t a_actorCount, const std::vector<std::string>& a_tags) const
	{
		std::vector<std::string> tagsLower;
		tagsLower.reserve(a_tags.size());
		for (const auto& tag : a_tags) {
			tagsLower.push_back(ToLower(tag));
		}

		std::shared_lock l{ lock };
		std::vector<std::string> result;
		for (const auto& [key, def] : animations) {
			if (def.actors.size() == a_actorCount && HasAllTags(def, tagsLower)) {
				result.push_back(def.id);
			}
		}
		std::sort(result.begin(), result.end());
		return result;
	}

	std::optional<PackRegistry::SlottedPick> PackRegistry::PickByTags(const std::vector<std::string>& a_tags,
		const std::vector<RE::SEX>& a_genders) const
	{
		std::vector<std::string> tagsLower;
		tagsLower.reserve(a_tags.size());
		for (const auto& tag : a_tags) {
			tagsLower.push_back(ToLower(tag));
		}

		std::shared_lock l{ lock };

		std::vector<const AnimationDef*> candidates;
		for (const auto& [key, def] : animations) {
			if (def.actors.size() == a_genders.size() && HasAllTags(def, tagsLower)) {
				candidates.push_back(&def);
			}
		}
		if (candidates.empty()) {
			return std::nullopt;
		}

		std::mt19937 rng{ std::random_device{}() };
		std::shuffle(candidates.begin(), candidates.end(), rng);

		// Party sizes are tiny (<= 4ish): brute-force the slot permutations.
		std::vector<size_t> perm(a_genders.size());
		for (const auto* def : candidates) {
			for (size_t i = 0; i < perm.size(); i++) {
				perm[i] = i;
			}
			do {
				if (SlotsSatisfied(def->actors, a_genders, perm)) {
					return SlottedPick{ def->id, perm };
				}
			} while (std::next_permutation(perm.begin(), perm.end()));
		}
		REX::WARN("PackRegistry: {} animation(s) match the tags but none fit these actors' genders", candidates.size());
		return std::nullopt;
	}

	size_t PackRegistry::Size() const
	{
		std::shared_lock l{ lock };
		return animations.size();
	}
}
