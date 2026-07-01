#pragma once

// Loads scenes from Data/OSF/**/*.osf.json. A scene is the one content concept: minimal (a `clip` or
// `stages[]` timeline, desugared to a node chain) up to a full graph of nodes with loop policy, edges,
// roles, and the four track lanes (cue/action/sound/camera). A node plays an inline `stages[]` timeline
// or `use`s another scene by id. Handles parsing, the desugar, validation, and load diagnostics.

#include "Animation/Scene.h"   // ParticipantPlacement, ScenePlan
#include "Input/InputTypes.h"  // PlayerControl capabilities default to Input::kAllCapabilities

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OSF::Registry
{
	// Scene schema version (*.osf.json declares this). Bump only on a breaking change.
	inline constexpr std::int64_t kSchemaVersion = 1;

	enum class SlotGender : std::uint8_t
	{
		kAny,
		kMale,
		kFemale
	};

	// Case-insensitive gender-string parse ("male"/"m" -> kMale, "female"/"f" -> kFemale, else kAny).
	SlotGender ParseSlotGender(std::string_view a_str);

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
		std::string  item;   // osf.equipment.equip: form ref "<plugin>|0xLOCAL" of the item to equip
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
	// A spec starting with '$' is a SoundRegistry pool query ("$tag,tag,..." — all-of) resolved to ONE clip at fire time (SceneRuntime::PlaySound), so a repeated/per-loop cue re-rolls; otherwise the value is taken literally.
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

	// One `camera` track entry: a held camera state, auto-restored on cleanup. States:
	// "thirdperson_hold" (force/hold third person via the standalone camera lock), "freefly" and
	// "vanity_orbit" (PlayerCamera state overrides). Also synthesized from a pack-level `camera`
	// default, attached to a scene's entry node.
	struct CameraEntry
	{
		CameraPos    pos = CameraPos::kEnter;
		float        fraction = 0.0f;
		bool         everyLoop = false;
		std::string  state;      // camera state id ("thirdperson_hold" / "freefly" / "vanity_orbit")
		float        distance = 0.0f;  // thirdperson_hold opening zoom pull-back (0 = engine default); ignored by other states
	};

	// One actor's clip for one stage (one per role in StageDef::clips, role order).
	struct StageClip
	{
		std::string file;
		std::string animId;
		std::optional<Animation::ParticipantPlacement> offset;  // overrides the role's default placement
	};

	// One stage of a timeline: timing + one clip per role. timer/loops 0 = no auto-advance (hold);
	// a stage that specifies NEITHER gets the play-once default (loops=1) at parse time.
	struct StageDef
	{
		std::string              name;          // browser label for this stage's animation ("" = none)
		std::vector<std::string> tags;          // per-stage tags (browse/filter); separate from scene tags
		float                    timer = 0.0f;  // seconds; 0 = no time-based auto-advance
		std::int32_t             loops = 0;     // clip loops before advancing; 0 = no loop-based auto-advance
		std::vector<StageClip>   clips;         // one per role, role order
		// Optional per-stage track lanes. DesugarLinear forwards each onto the stage's synthetic node,
		// where the runtime's dispatch reads them — so a linear stage can carry cues, actions, audio,
		// and camera postures without dropping to the full nodes[] graph form.
		std::vector<CueEntry>    cues;
		std::vector<ActionEntry> actions;
		std::vector<SoundEntry>  sounds;
		std::vector<CameraEntry> cameras;
	};

	struct SceneNode
	{
		std::string              id;
		// A node carries EXACTLY ONE playable:
		std::string              use;          //   reference another scene by id, OR
		std::vector<StageDef>    stages;       //   an inline clip timeline (one-off, no separate file)
		LoopMode                 loopMode = LoopMode::kOnce;  // JSON `loops`: omit=once, 0=hold, N=count
		std::int32_t             loopCount = 0;              // when loopMode == kCount
		float                    timerSec = 0.0f;            // JSON `timer` (seconds)
		std::vector<SceneEdge>   edges;
		std::vector<CueEntry>    cues;          // `cue` track
		std::vector<ActionEntry> actions;       // `action` track
		std::vector<SoundEntry>  sounds;        // `sound` track
		std::vector<CameraEntry> cameras;       // `camera` track
	};

	// Item(s) to equip onto a role's bound actor at scene start, keyed by the actor's gender, and
	// auto-removed on every end path (the kEquipItem ledger). Authored as `equip` on the role: a bare
	// form-ref string (any gender) or an object { male?, female?, any? }. Stored as "<Plugin>|0xLOCAL"
	// refs and resolved at FIRE time (game thread): a ref naming an uninstalled plugin warns + is
	// skipped, it does NOT reject the scene — these usually point at optional body-replacer plugins.
	struct RoleEquip
	{
		std::string male;
		std::string female;
		std::string any;  // fallback when the bound actor's gendered key is absent (and for a bare string)

		[[nodiscard]] bool Empty() const noexcept { return male.empty() && female.empty() && any.empty(); }

		// The ref to equip for an actor whose gender tag is a_gender ("male"/"female"/"" agnostic):
		// the gendered key if present, else `any`. The returned ref may be "" (caller skips).
		[[nodiscard]] const std::string& ForGender(std::string_view a_gender) const noexcept
		{
			if (a_gender == "male" && !male.empty()) {
				return male;
			}
			if (a_gender == "female" && !female.empty()) {
				return female;
			}
			return any;
		}
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
		Animation::ParticipantPlacement offset{};  // default placement for this slot
		RoleEquip                    equip;     // optional per-gender item equipped for the scene's duration
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
		bool                     unlisted = false;  // excluded from the matchmaking pool; only reachable by direct id
		bool                     lockPlayer = true; //Player input disabled by default when player participant
		bool                     stripActors = true;  // Remove every participant's worn apparel by default (base skin kept), auto-restored on end;
		bool                     fade = false;  // Screen fade-to-black on start when the player participates (self-releasing curtain); OFF by default, opt in with `fade:true`
		PlayerControl            playerControl;  // director-input grant; ENABLED by default, scene opts out/narrows
		std::vector<std::string> tags;
		std::vector<SceneRole>   roles;
		std::string              entry;
		std::vector<SceneNode>   nodes;
		std::vector<std::string> linearStages;  // optional: stage i -> node id (GetSceneStage/SetSceneStage)

		std::filesystem::path    sourceFile;

		// When anchorKeywords/anchorBaseForms set, the scene is ANCHOR-BOUND. can only start anchored to ref with base form OR has keyword.
		std::vector<RE::BGSKeyword*>    anchorKeywords;   // resolved at load (any-of); empty = no keyword match
		std::vector<RE::TESFormID>      anchorBaseForms;  // resolved at load (any-of); empty = no base match
		Animation::ParticipantPlacement anchorOffset{};

		[[nodiscard]] bool RequiresAnchor() const noexcept { return !anchorKeywords.empty() || !anchorBaseForms.empty(); }

		const SceneNode* FindNode(std::string_view a_id) const;

		// Index of a_nodeId in linearStages (case-insensitive), or -1 (also -1 if the scene declares no linearStages — a non-linear graph has no stage number).
		std::int32_t LinearStageOf(std::string_view a_nodeId) const;
	};

	class SceneRegistry
	{
	public:
		static SceneRegistry& GetSingleton();

		// Scans Data/OSF/**/*.osf.json and rebuilds the registry.
		// Bad scenes are skipped; every skip and warning is both logged and recorded for LoadErrors().
		// Runs at startup and again on OSF.ReloadPacks().
		void LoadAll();

		// Scene by id (case-insensitive), or nullptr.
		const SceneDef* Find(std::string_view a_id) const;

		// Resolve a node's inline `stages`, or a `use` target's single inline-stage node - to a ScenePlan (files + placements + timer/loops), or nullopt (reason logged).
		// a_actorCount must equal the resolved role count.
		std::optional<Animation::ScenePlan> BuildNodePlan(const SceneDef& a_def, const SceneNode& a_node, size_t a_actorCount) const;

		// Visit every loaded scene def under the read lock (used by the matchmaker to build candidates). 
		// The callback runs while the registry lock is held, so it must NOT re-enter the registry; keep it to reading the def + cheap per-actor predicate checks.
		void ForEachDef(const std::function<void(const SceneDef&)>& a_fn) const;

		size_t Size() const;

		// Problems (errors + warnings) from the last LoadAll, for OSF.GetSceneLoadErrors().
		std::vector<std::string> LoadErrors() const;

		// Data-relative clip references from loaded scenes whose resolved file does not currently exist.
		std::vector<std::string> MissingClipRefs() const;

	private:
		mutable std::shared_mutex        lock;
		std::unordered_map<std::string, SceneDef> scenes;  // key = lowercased id
		std::vector<std::string>         loadErrors;
	};
}
