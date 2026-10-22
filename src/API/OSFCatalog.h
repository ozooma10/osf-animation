// Schema-4 catalog and schema-2 clip-source input. Serialized to UTF-8 JSON, never passed across a DLL ABI.
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
		std::uint32_t stage{}, clip{};
		float seconds{};
		bool available{};
		OSF_CATALOG_FIELDS(SourceClip, source, file, animId, display, species, stage, clip, seconds, available)
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
	struct Node {
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
		OSF_CATALOG_FIELDS(Node, id, name, species, pack, folder, sourceFile, sourcePath, sourceKind,
			placement, priority, weight, actorCount, unlisted, curatedClip, library, clipsAvailable,
			playerInputLock, hideApparel, fade, tags, roles, stages, anchorKeywords, anchorBaseForms, provider)
	};
	struct Problem {
		bool warning{};
		std::string code, message, hint, node, role, clip;
		OSF_CATALOG_FIELDS(Problem, warning, code, message, hint, node, role, clip)
	};
	struct File {
		std::string file, path, pack;
		bool library{};
		std::int64_t schema{};
		std::uint64_t bytes{};
		float parseMs{};
		std::uint32_t declaredNodes{}, hidden{}, rejectedNodes{}, unlisted{}, anchored{}, nodes{},
			stages{}, roles{}, clips{}, distinctClips{}, missingClips{}, cues{}, actions{}, sounds{}, cameras{},
			clipEntries{}, errors{}, warnings{};
		std::vector<std::string> species, missingClipExamples;
		std::vector<Problem> problems;
		bool Rejected() const { return nodes == 0 && clipEntries == 0 && errors > 0; }
		OSF_CATALOG_FIELDS(File, file, path, pack, library, schema, bytes, parseMs,
			declaredNodes, hidden, rejectedNodes, unlisted, anchored, nodes, stages, roles, clips,
			distinctClips, missingClips, cues, actions, sounds, cameras, clipEntries, errors, warnings,
			species, missingClipExamples, problems)
	};
	struct Snapshot {
		std::uint32_t schema{4};
		std::vector<File> files;
		std::vector<std::string> errors;
		// Unique unmeasured clip references across all nodes.
		std::vector<Clip> clips;
		std::vector<SourceClip> sourceClips;
		std::vector<Node> nodes;
		OSF_CATALOG_FIELDS(Snapshot, schema, files, errors, clips, sourceClips, nodes)
	};
	// Compact schema-2 provider input: the same global IDs and clip addresses, without
	// presentation stages, duration queries or import diagnostics. All node IDs are reserved.
	struct ClipSourceNode {
		std::string id, sourceFile, pack, provider;
		std::vector<std::string> tags;
		bool library{}, curatedClip{};
		OSF_CATALOG_FIELDS(ClipSourceNode, id, sourceFile, pack, provider, tags, library, curatedClip)
	};
	struct ClipSourceSnapshot {
		std::uint32_t schema{2};
		std::vector<ClipSourceNode> nodes;
		std::vector<SourceClip> sourceClips;
		OSF_CATALOG_FIELDS(ClipSourceSnapshot, schema, nodes, sourceClips)
	};
}

#undef OSF_CATALOG_FIELDS
#undef OSF_CATALOG_VISIT
