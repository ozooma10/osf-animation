// Schema-3 catalog and schema-1 clip-source input. Serialized to UTF-8 JSON, never passed across a DLL ABI.
// Parsing produces caller-owned copies; sourcePath is relative to Data/OSF. Unknown fields may be ignored.
#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <map>

// Keep the DOM converters and const/mutable streaming visitors on one schema declaration.
#define OSF_CATALOG_VISIT(field) visitor.Field(#field, value.field);
#define OSF_CATALOG_FIELDS(Type, ...) \
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(Type, __VA_ARGS__) \
    template <class Visitor> friend void VisitFields(Visitor& visitor, const Type& value) { \
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(OSF_CATALOG_VISIT, __VA_ARGS__)) } \
    template <class Visitor> friend void VisitFields(Visitor& visitor, Type& value) { \
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(OSF_CATALOG_VISIT, __VA_ARGS__)) }

namespace OSF::Catalog
{
	struct SourceClip {
		std::string source, file, animId, display, species;
		std::uint32_t node{}, stage{}, clip{};
		float seconds{};
		bool available{};
		OSF_CATALOG_FIELDS(SourceClip, source, file, animId, display, species, node, stage, clip, seconds, available)
	};
	struct Clip {
		std::string key, file, animId;
		OSF_CATALOG_FIELDS(Clip, key, file, animId)
	};
	struct DecodedDuration {
		float sec{};
		std::uint64_t revision{};
		OSF_CATALOG_FIELDS(DecodedDuration, sec, revision)
	};
	using DecodedDurations = std::map<std::string, DecodedDuration>;
	struct Role {
		std::string name, gender;
		OSF_CATALOG_FIELDS(Role, name, gender)
	};
	struct Stage {
		std::int32_t index{}, clipCount{}, loopCount{};
		std::string name, loopMode;
		std::vector<std::string> tags;
		float loopSec{-1}, timerSec{}, hold{-1};
		bool resolved{};
		std::string durationKey;
		OSF_CATALOG_FIELDS(Stage, index, clipCount, loopCount, name, loopMode, tags, loopSec, timerSec, hold, resolved, durationKey)
	};
	struct Scene {
		std::string provider;
		std::string id, name, species, pack, folder, sourceFile, sourcePath, sourceKind, placement;
		std::int32_t priority{}, weight{1};
		std::uint32_t actorCount{};
		bool unlisted{}, curatedClip{}, library{}, clipsAvailable{true}, playerInputLock{true}, hideApparel{true}, fade{};
		std::vector<std::string> tags;
		std::vector<Role> roles;
		std::vector<Stage> stages;
		std::vector<std::uint32_t> anchorKeywords, anchorBaseForms;
		bool RequiresAnchor() const { return !anchorKeywords.empty() || !anchorBaseForms.empty(); }
		OSF_CATALOG_FIELDS(Scene, id, name, species, pack, folder, sourceFile, sourcePath, sourceKind,
			placement, priority, weight, actorCount, unlisted, curatedClip, library, clipsAvailable,
			playerInputLock, hideApparel, fade, tags, roles, stages, anchorKeywords, anchorBaseForms, provider)
	};
    // Address is the exact (scene, id) pair. Retains the containing route and its edges.
    struct Node {
        std::string scene, id, name;
        std::vector<std::string> tags;
        std::int32_t stage{-1}, priority{}, weight{1};
        std::uint32_t segments{};
        bool entry{}, unlisted{}, clipsAvailable{};
        OSF_CATALOG_FIELDS(Node, scene, id, name, tags, stage, priority, weight, segments, entry, unlisted, clipsAvailable)
    };
	struct Problem {
		bool warning{};
		std::string code, message, hint, scene, node, role, clip;
		OSF_CATALOG_FIELDS(Problem, warning, code, message, hint, scene, node, role, clip)
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
		OSF_CATALOG_FIELDS(File, file, path, pack, library, schema, bytes, parseMs, scenes,
			declaredScenes, hidden, rejectedScenes, unlisted, anchored, nodes, stages, roles, clips,
			distinctClips, missingClips, cues, actions, sounds, cameras, clipEntries, errors, warnings,
			species, missingClipExamples, problems)
	};
	struct Snapshot {
		std::uint32_t schema{3};
		std::vector<Scene> scenes;
		std::vector<File> files;
		std::vector<std::string> errors;
		// Unique unmeasured clip references across all nodes, including nonlinear scenes.
		std::vector<Clip> clips;
		std::vector<SourceClip> sourceClips;
		std::vector<Node> nodes;
		OSF_CATALOG_FIELDS(Snapshot, schema, scenes, files, errors, clips, sourceClips, nodes)
	};
	// Optional schema-1 provider input (services 3.17). It describes the same base content as
	// Snapshot, without presentation nodes/stages, duration queries or import diagnostics.
	// All scenes are included, including those without clips, so authored IDs can be reserved.
	struct ClipSourceScene {
		std::string id, sourceFile, pack, provider;
		std::vector<std::string> tags;
		bool library{}, curatedClip{};
		OSF_CATALOG_FIELDS(ClipSourceScene, id, sourceFile, pack, provider, tags, library, curatedClip)
	};
	struct ClipSourceSnapshot {
		std::uint32_t schema{1};
		std::vector<ClipSourceScene> scenes;
		std::vector<SourceClip> sourceClips;
		OSF_CATALOG_FIELDS(ClipSourceSnapshot, schema, scenes, sourceClips)
	};
}

#undef OSF_CATALOG_FIELDS
#undef OSF_CATALOG_VISIT
