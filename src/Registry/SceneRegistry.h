#pragma once

// Loads scene graphs from Data/OSF/**/*.scene.json (docs/SCENE_DESIGN.md §1.3). A scene is
// a graph of nodes; each node references an animation id (from PackRegistry) + a loop policy
// + outgoing edges. This is the Phase-A MVP: graph structure (nodes/edges/roles/loop/timer)
// + validation + load diagnostics. Tracks (sound/cue/action/camera) are deferred.

#include "Registry/PackRegistry.h"  // SlotGender

namespace OSF::Registry
{
	// Highest scene "schema" we parse. Bump only on a breaking change.
	inline constexpr std::int64_t kSceneSchemaVersion = 1;

	enum class LoopMode : std::uint8_t
	{
		kOnce,   // play through, then the "end" edge
		kHold,   // loop until "advance"
		kCount   // loopCount loops, then the "loops" edge
	};

	enum class EdgeWhen : std::uint8_t
	{
		kEnd,
		kLoops,
		kTimer,
		kAdvance,
		kTrigger  // trigger:<cue>
	};

	struct SceneEdge
	{
		std::string  id;        // "" for auto-edges; required on branchable (advance) edges
		std::string  label;     // required on branchable edges
		std::string  labelKey;  // optional localization key
		std::string  to;        // target node id, or "$end"
		EdgeWhen     when = EdgeWhen::kAdvance;
		std::string  trigger;   // cue id (when == kTrigger)
		bool         isDefault = false;
		std::int32_t priority = 0;
	};

	// Where on the node's clip timeline a cue fires. kFraction = clip-local fraction in [0,1);
	// the rest are the named lifecycle anchors (§1.3 time model).
	enum class CuePos : std::uint8_t
	{
		kFraction,
		kEnter,
		kExit,
		kEnd
	};

	// One `cue` track entry: fires EVENT_CUE (and, later, drives a trigger:<id> edge).
	struct CueEntry
	{
		CuePos       pos = CuePos::kEnter;
		float        fraction = 0.0f;   // when pos == kFraction
		bool         everyLoop = false;  // repeat:"loop" (numeric only)
		std::string  id;
	};

	struct SceneNode
	{
		std::string              id;
		std::string              anim;         // referenced animation id (PackRegistry)
		std::vector<std::string> slots;        // role->slot map (optional; else declaration order)
		LoopMode                 loopMode = LoopMode::kHold;
		std::int32_t             loopCount = 0;  // when loopMode == kCount
		float                    timerSec = 0.0f;
		bool                     loopForever = false;
		std::vector<SceneEdge>   edges;
		std::vector<CueEntry>    cues;          // `cue` track (sound/action/camera lanes deferred)
	};

	struct SceneRole
	{
		std::string name;
		SlotGender  gender = SlotGender::kAny;
	};

	struct SceneDef
	{
		std::string              id;
		std::string              name;
		std::int32_t             priority = 0;
		std::vector<std::string> tags;
		std::vector<SceneRole>   roles;
		std::string              entry;
		std::vector<SceneNode>   nodes;
		std::vector<std::string> linearStages;  // optional: stage i -> node id (GetSceneStage/SetSceneStage)
		std::filesystem::path    sourceFile;

		const SceneNode* FindNode(std::string_view a_id) const;

		// Index of a_nodeId in linearStages (case-insensitive), or -1 (also -1 if the scene
		// declares no linearStages — a non-linear graph has no stage number).
		std::int32_t LinearStageOf(std::string_view a_nodeId) const;
	};

	class SceneRegistry
	{
	public:
		static SceneRegistry& GetSingleton();

		// Scans Data/OSF/**/*.scene.json and replaces the registry. Bad scenes are skipped;
		// every skip/warning is logged AND recorded for LoadErrors(). Called after the pack
		// registry at kPostDataLoad and from OSF.ReloadPacks().
		void LoadAll();

		// Scene by id (case-insensitive), or nullptr.
		const SceneDef* Find(std::string_view a_id) const;

		size_t Size() const;

		// Problems (errors + warnings) from the last LoadAll, for OSF.GetSceneLoadErrors().
		std::vector<std::string> LoadErrors() const;

	private:
		mutable std::shared_mutex        lock;
		std::unordered_map<std::string, SceneDef> scenes;  // key = lowercased id
		std::vector<std::string>         loadErrors;
	};
}
