#pragma once

// Loads scene graphs from Data/OSF/**/*.scene.json. A scene is a graph of nodes; 
// each node references an animation id (from PackRegistry), a loop policy, and outgoing edges, plus the four track lanes (cue/action/sound/camera). 
// Handles the graph structure, validation, and load diagnostics.

#include "Input/InputTypes.h"  // PlayerControl capabilities default to Input::kAllCapabilities
#include "Registry/PackRegistry.h"

#include <functional>

namespace OSF::Registry
{
	// Highest scene schema version we understand. Bump only on a breaking change.
	inline constexpr std::int64_t kSceneSchemaVersion = 1;

	// Unified scene schema version 
	inline constexpr std::int64_t kUnifiedSchemaVersion = 2;

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

	// Where on the node's clip timeline a cue fires. kFraction = a clip-local fraction in [0,1); the rest are the named lifecycle anchors.
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

	// Where an `action` track entry runs. kEnter/kExit are the lifecycle anchors, fired directly on node enter/exit; 
	// kFraction/kEnd are timed by the clip clock, fired through the same timed-mark path as cues.
	enum class ActionPos : std::uint8_t
	{
		kEnter,
		kExit,
		kFraction,
		kEnd
	};

	// One `action` track entry: a namespaced mechanism. `osf.*` types are built-in (run by the  runtime); 
	// any other namespace is a custom action emitted as EVENT_ACTION (notification).
	struct ActionEntry
	{
		ActionPos    pos = ActionPos::kEnter;
		float        fraction = 0.0f;    // when pos == kFraction
		bool         everyLoop = false;  // repeat:"loop" (numeric only)
		std::string  type;   // namespaced (osf.* built-in, else custom)
		std::string  role;   // optional role the action targets
		bool         hold = false;       // osf.fade.out: end faded (opt out of the cleanup fade-in)
		float        duration = 0.0f;    // osf.fade.*: ramp seconds (0 = mechanism default)
		std::string  set;    // osf.voice.play: sound spec (Data-relative path or "event:<name>")
	};

	// Where a `sound` track entry fires — same time model as cues and actions. kEnter/kExit are lifecycle anchors; kFraction/kEnd are clip-timed.
	enum class SoundPos : std::uint8_t
	{
		kEnter,
		kExit,
		kFraction,
		kEnd
	};

	// One `sound` track entry: play a sound spec. `spec` is a Data-relative file path (played through miniaudio) or an "event:<name>"/"event:0x<id>" Wwise spec (engine-mixed). 
	// The schema's `pool` name -> clip resolution isn't wired up yet, so for now the value is taken as a literal spec.
	struct SoundEntry
	{
		SoundPos     pos = SoundPos::kFraction;
		float        fraction = 0.0f;
		bool         everyLoop = false;
		std::string  spec;    // file path or event: spec
		std::string  role;    // optional; positions the sound at this actor (else the player)
		float        volume = 1.0f;
	};

	// Where a `camera` track entry fires — same time model as the other lanes.
	enum class CameraPos : std::uint8_t
	{
		kEnter,
		kExit,
		kFraction,
		kEnd
	};

	// One `camera` track entry: a held camera state, auto-restored on cleanup. 
	// For now the only state is "thirdperson_hold" (force and hold third person via the standalone camera lock);
	// free-fly/orbit/matrix states aren't supported yet.
	struct CameraEntry
	{
		CameraPos    pos = CameraPos::kEnter;
		float        fraction = 0.0f;
		bool         everyLoop = false;
		std::string  state;   // camera state id ("thirdperson_hold")
	};

	struct SceneNode
	{
		std::string              id;
		std::string              anim;         // LEGACY (*.scene.json): referenced animation id (PackRegistry)
		// Unified (*.osf.json) playables — a node carries EXACTLY ONE of:
		std::string              use;          //   reference another scene by id (the old `anim`, renamed), OR
		std::vector<StageDef>    stages;       //   an inline clip timeline (one-off, no separate file)
		LoopMode                 loopMode = LoopMode::kHold;
		std::int32_t             loopCount = 0;  // when loopMode == kCount
		float                    timerSec = 0.0f;
		bool                     loopForever = false;
		std::vector<SceneEdge>   edges;
		std::vector<CueEntry>    cues;          // `cue` track
		std::vector<ActionEntry> actions;       // `action` track
		std::vector<SoundEntry>  sounds;        // `sound` track
		std::vector<CameraEntry> cameras;       // `camera` track
	};

	struct SceneRole
	{
		std::string name;  // "" = an anonymous positional slot (unified *.osf.json; clips index-align to role order)
		SlotGender  gender = SlotGender::kAny;
		// Resolved role filters (resolved once at scene load via the form-ref resolver).
		// The role's bound actor must satisfy every PRESENT constraint;
		// within `keywords`/`races` it is any-of (the actor needs ANY listed keyword, and ANY listed race).
		// An empty vector = that constraint is absent. `gender` desugars from `gender`/`filters.gender`.
		std::vector<RE::BGSKeyword*> keywords;  // empty = no keyword constraint
		std::vector<RE::TESRace*>    races;     // empty = no race constraint
		Animation::ParticipantPlacement offset{};  // default placement for this slot (was pack SlotDef::offset)
	};

	// Per-scene player-input grant. Input control is ENABLED BY DEFAULT: with no `playerControl` block the player gets every capability while participating. 
	// A scene opts out wholesale (`"playerControl": false`) or narrows it (`{ "disable": ["speed","end"], "locked": true }`).
	// capabilities is an OR of OSF::Input::Capability bits (advance/navigate/speed/reposition/freecam/end).
	struct PlayerControl
	{
		bool          enabled = true;                  // false => no input channel at all
		std::uint32_t capabilities = Input::kAllCapabilities;  // capabilities granted (default: all; `disable` removes)
		std::string   controlRole;                     // role whose scene the local input drives ("" => the player participant)
		bool          locked = false;                  // player may not end the scene via the input channel (story scenes)
	};

	struct SceneDef
	{
		std::string              id;
		std::string              name;
		std::int32_t             priority = 0;
		std::int32_t             weight = 1;  // weighted-random sampling within the top priority tier (StartSceneByTags*)
		bool                     lockPlayer = true; //Player input disabled by default when player participant
		bool                     stripActors = true;  // Remove every participant's worn apparel by default (base skin kept), auto-restored on end;
		PlayerControl            playerControl;  // director-input grant; ENABLED by default, scene opts out/narrows
		std::vector<std::string> tags;
		std::vector<SceneRole>   roles;
		std::string              entry;
		std::vector<SceneNode>   nodes;
		std::vector<std::string> linearStages;  // optional: stage i -> node id (GetSceneStage/SetSceneStage)
		std::filesystem::path    sourceFile;

		const SceneNode* FindNode(std::string_view a_id) const;

		// Index of a_nodeId in linearStages (case-insensitive), or -1 (also -1 if the scene declares no linearStages — a non-linear graph has no stage number).
		std::int32_t LinearStageOf(std::string_view a_nodeId) const;
	};

	class SceneRegistry
	{
	public:
		static SceneRegistry& GetSingleton();

		// Scans Data/OSF/**/*.scene.json and rebuilds the registry. 
		// Bad scenes are skipped; every skip and warning is both logged and recorded for LoadErrors().
		// Runs after the pack registry at startup, and again on OSF.ReloadPacks().
		void LoadAll();

		// Scene by id (case-insensitive), or nullptr.
		const SceneDef* Find(std::string_view a_id) const;

		// Visit every loaded scene def under the read lock (used by the matchmaker to build candidates). 
		// The callback runs while the registry lock is held, so it must NOT re-enter the registry; keep it to reading the def + cheap per-actor predicate checks.
		void ForEachDef(const std::function<void(const SceneDef&)>& a_fn) const;

		size_t Size() const;

		// Problems (errors + warnings) from the last LoadAll, for OSF.GetSceneLoadErrors().
		std::vector<std::string> LoadErrors() const;

	private:
		mutable std::shared_mutex        lock;
		std::unordered_map<std::string, SceneDef> scenes;  // key = lowercased id
		std::vector<std::string>         loadErrors;
	};
}
