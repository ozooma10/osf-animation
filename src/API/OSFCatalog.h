// Schema-1 catalog value types. These are serialized to UTF-8 JSON, never passed across a DLL ABI.
// Parsing produces caller-owned copies; sourcePath is relative to Data/OSF. Unknown fields may be ignored.
#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace OSF::Catalog
{
	struct Role {
		std::string name, gender;
		NLOHMANN_DEFINE_TYPE_INTRUSIVE(Role, name, gender)
	};
	struct Stage {
		std::int32_t index{}, clipCount{}, loopCount{};
		std::string name, loopMode;
		std::vector<std::string> tags;
		float loopSec{-1}, timerSec{}, hold{-1};
		bool resolved{};
		NLOHMANN_DEFINE_TYPE_INTRUSIVE(Stage, index, clipCount, loopCount, name, loopMode, tags, loopSec, timerSec, hold, resolved)
	};
	struct Scene {
		std::string id, name, species, pack, folder, sourceFile, sourcePath, sourceKind, placement;
		std::int32_t priority{}, weight{1};
		std::uint32_t actorCount{};
		bool unlisted{}, curatedClip{}, library{}, clipsAvailable{true}, playerInputLock{true}, hideApparel{true}, fade{};
		std::vector<std::string> tags;
		std::vector<Role> roles;
		std::vector<Stage> stages;
		std::vector<std::uint32_t> anchorKeywords, anchorBaseForms;
		bool RequiresAnchor() const { return !anchorKeywords.empty() || !anchorBaseForms.empty(); }
		NLOHMANN_DEFINE_TYPE_INTRUSIVE(Scene, id, name, species, pack, folder, sourceFile, sourcePath, sourceKind,
			placement, priority, weight, actorCount, unlisted, curatedClip, library, clipsAvailable,
			playerInputLock, hideApparel, fade, tags, roles, stages, anchorKeywords, anchorBaseForms)
	};
	struct Problem {
		bool warning{};
		std::string code, message, hint, scene, node, role, clip;
		NLOHMANN_DEFINE_TYPE_INTRUSIVE(Problem, warning, code, message, hint, scene, node, role, clip)
	};
	struct File {
		std::string file, path, pack;
		bool library{};
		std::int64_t schema{};
		std::uint64_t bytes{};
		float parseMs{};
		std::uint32_t scenes{}, declaredScenes{}, hidden{}, rejectedScenes{}, unlisted{}, anchored{}, nodes{},
			stages{}, roles{}, clips{}, distinctClips{}, missingClips{}, cues{}, actions{}, sounds{}, cameras{},
			clipEntries{}, errors{}, warnings{};
		std::vector<std::string> species, missingClipExamples;
		std::vector<Problem> problems;
		bool Rejected() const { return scenes == 0 && clipEntries == 0 && errors > 0; }
		NLOHMANN_DEFINE_TYPE_INTRUSIVE(File, file, path, pack, library, schema, bytes, parseMs, scenes,
			declaredScenes, hidden, rejectedScenes, unlisted, anchored, nodes, stages, roles, clips,
			distinctClips, missingClips, cues, actions, sounds, cameras, clipEntries, errors, warnings,
			species, missingClipExamples, problems)
	};
	struct Snapshot {
		std::uint32_t schema{1};
		std::vector<Scene> scenes;
		std::vector<File> files;
		std::vector<std::string> errors;
		NLOHMANN_DEFINE_TYPE_INTRUSIVE(Snapshot, schema, scenes, files, errors)
	};
}
