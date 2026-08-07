#pragma once

// Loads scenes from Data/OSF/**/*.osf.json. A scene is the one content concept: minimal (a `clip` or
// `stages[]` timeline, desugared to a node chain) up to a full graph of nodes with loop policy, edges,
// roles, and the four track lanes (cue/action/sound/camera). A node plays an inline `stages[]` timeline
// or `use`s another scene by id. Handles parsing, the desugar, validation, and load diagnostics.

#include "Animation/Scene.h"   // ParticipantPlacement, PlaybackPlan
#include "Input/InputTypes.h"  // SceneControls capabilities default to Input::kAllCapabilities
#include "Props/PropTypes.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace OSF::Registry
{
	// Scene schema version (*.osf.json declares this). Bump only on a breaking change.
	inline constexpr std::int64_t kSchemaVersion = 1;

	enum class RoleGender : std::uint8_t
	{
		kAny,
		kMale,
		kFemale
	};
	using SlotGender = RoleGender;  // compatibility spelling

	// Case-insensitive gender-string parse ("male"/"m" -> kMale, "female"/"f" -> kFemale, else kAny).
	RoleGender ParseRoleGender(std::string_view a_str);
	SlotGender ParseSlotGender(std::string_view a_str);  // compatibility forwarding symbol

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

	// Shared position model for all four node tracks. Aliases below preserve the lane-specific
	// names at call sites while keeping parsing/scheduling semantics structurally identical.
	enum class TrackPos : std::uint8_t
	{
		kEnter,
		kExit,
		kFraction,
		kEnd
	};
	using CuePos = TrackPos;
	using ActionPos = TrackPos;
	using SoundPos = TrackPos;
	using CameraPos = TrackPos;

	// `atFrame` frame rate. An authored frame is a clip-local position at the Creation-Engine 30 fps
	// convention (AFImport::kAfFps, the rate an `.af` decodes at, and the rate the studio route
	// compiler bakes its `at` fractions with), so frame N is exactly N / 30 clip-local SECONDS —
	// independent of how long the clip turns out to be, unlike a fraction.
	inline constexpr float kFrameRate = 30.0f;

	enum class SoundEmitter : std::uint8_t
	{
		kListener,
		kRole
	};

	// One `cue` track entry: fires EVENT_CUE (and, later, drives a trigger:<id> edge).
	struct CueEntry
	{
		CuePos       pos = CuePos::kEnter;
		float        fraction = 0.0f;   // when pos == kFraction and frame < 0
		float        frame = -1.0f;     // when pos == kFraction: authored `atFrame` (< 0 = authored as a fraction)
		bool         everyLoop = false;  // repeat:"loop" (numeric only)
		std::string  id;
	};

	enum class ActionKind : std::uint8_t
	{
		kCustom,
		kControlLock, kControlRelease,
		kEquipmentHide, kEquipmentRestore, kEquipmentEquip, kEquipmentUnequip,
		kWeaponSheathe, kWeaponRestore,
		kFadeOut, kFadeIn,
		kVoicePlay,
		kPropAttach, kPropDestroy
	};

	// One `action` track entry: a namespaced mechanism. `osf.*` types are built-in (run by the runtime);
	// any other namespace is a custom action emitted as EVENT_ACTION (notification).
	struct ActionEntry
	{
		ActionPos    pos = ActionPos::kEnter;
		float        fraction = 0.0f;    // when pos == kFraction and frame < 0
		float        frame = -1.0f;      // when pos == kFraction: authored `atFrame` (< 0 = authored as a fraction)
		bool         everyLoop = false;  // repeat:"loop" (numeric only)
		std::string  type;   // namespaced (osf.* built-in, else custom)
		ActionKind   kind = ActionKind::kCustom;
		std::string  role;   // optional role the action targets
		bool         hold = false;       // osf.fade.out: end faded (opt out of the cleanup fade-in)
		float        duration = 0.0f;    // osf.fade.*: ramp seconds (0 = mechanism default)
		std::string  set;    // osf.voice.play: sound spec (Data-relative path or "event:<name>")
		SoundEmitter emitter = SoundEmitter::kListener;  // osf.voice.play: listener (default) or role actor
		std::string  item;   // osf.equipment.equip: form ref "<plugin>|0xLOCAL" of the item to equip
		std::string  prop;    // osf.prop.*: scene-local prop id
		Props::Source     propSource;      // osf.prop.attach: fixed form or equipped-armor selector
		Props::Attachment propAttachment;  // osf.prop.attach: actor node + local transform
	};

	// One `sound` track entry: play a sound spec. `spec` is a Data-relative file path (played through miniaudio) or an "event:<name>"/"event:0x<id>" Wwise spec (engine-mixed).
	// A spec starting with '$' is a SoundRegistry pool query ("$tag,tag,..." — all-of) resolved to ONE clip at fire time (SceneRuntime::PlaySound), so a repeated/per-loop cue re-rolls; otherwise the value is taken literally.
	struct SoundEntry
	{
		SoundPos     pos = SoundPos::kFraction;
		float        fraction = 0.0f;
		float        frame = -1.0f;  // authored `atFrame` (< 0 = authored as a fraction)
		bool         everyLoop = false;
		std::string  spec;    // file path or event: spec
		std::string  role;    // optional voice channel, gender source, and subtitle speaker
		SoundEmitter emitter = SoundEmitter::kListener;  // Wwise listener (default) or positioned role actor
	};

	// One `camera` track entry: a held camera state, auto-restored on cleanup.
	enum class CameraState : std::uint8_t
	{
		kNone,
		kThirdPersonHold,
		kFreeFly,
		kVanityOrbit,
		kSceneOrbit
	};
	std::optional<CameraState> ParseCameraState(std::string_view a_state);
	std::string_view CameraStateName(CameraState a_state);

	// "thirdperson_hold" (force/hold third person via the standalone camera lock), "freefly" and
	// "vanity_orbit" (PlayerCamera state overrides). Also synthesized from a file-level `camera`
	// default, attached to a scene's entry node.
	struct CameraEntry
	{
		CameraPos    pos = CameraPos::kEnter;
		float        fraction = 0.0f;
		float        frame = -1.0f;  // authored `atFrame` (< 0 = authored as a fraction)
		bool         everyLoop = false;
		CameraState  state = CameraState::kNone;
		float        distance = 0.0f;  // thirdperson_hold opening zoom pull-back (0 = engine default); ignored by other states
	};

	// Clip-local seconds for a track entry authored with `atFrame`; -1 when it was authored with a
	// fraction `at` (which only means something once the clip duration is known).
	template <class Entry>
	constexpr float TrackSeconds(const Entry& a_entry)
	{
		return a_entry.frame >= 0.0f ? a_entry.frame / kFrameRate : -1.0f;
	}

	// A kFraction entry's position as a clip fraction. An `atFrame` entry needs the clip duration to
	// place itself on that axis; with no duration to scale against (a_durationSec <= 0) only frame 0
	// has a knowable place, so later frames report the clip end rather than pretending to be at the start.
	template <class Entry>
	inline float TrackFraction(const Entry& a_entry, float a_durationSec)
	{
		const float sec = TrackSeconds(a_entry);
		if (sec < 0.0f) {
			return a_entry.fraction;
		}
		if (a_durationSec > 0.0f) {
			return std::clamp(sec / a_durationSec, 0.0f, 1.0f);
		}
		return sec > 0.0f ? 1.0f : 0.0f;
	}

	// Whether the runtime can ever fire this kFraction entry. Scene::Advance fires numeric marks
	// through a [prev, next) window whose upper bound never exceeds the clip duration, so an
	// `atFrame` at or past the clip end never fires — while the clamped TrackFraction above would
	// still report it at 1.0, letting an inspector scrubbed to the end "fire" a mark the shipped
	// scene provably skips. `at`-fraction entries and unknown durations are not judged (true).
	template <class Entry>
	inline bool TrackFires(const Entry& a_entry, float a_durationSec)
	{
		const float sec = TrackSeconds(a_entry);
		return sec < 0.0f || a_durationSec <= 0.0f || sec < a_durationSec;
	}

	// One actor's clip for one stage (one per role in StageDef::clips, role order).
	struct StageClip
	{
		std::string file;
		std::string animId;
		float       sec = 0.0f;  // pack-authored clip duration in seconds (0 = unknown, probe at runtime)
		std::optional<Animation::ParticipantPlacement> offset;  // overrides the role's default placement
		std::optional<std::string> mask;  // overrides the role's bone mask for this stage
	};

	// One stage of a timeline: timing + one clip per role. timer/loops 0 = no auto-advance (hold);
	// a stage that specifies NEITHER gets the play-once default (loops=1) at parse time.
	struct StageDef
	{
		std::string              name;          // browser label for this stage's animation ("" = none)
		std::vector<std::string> tags;          // per-stage tags (browse/filter); separate from scene tags
		float                    timer = 0.0f;  // seconds; 0 = no time-based auto-advance
		std::int32_t             loops = 0;     // clip loops before advancing; 0 = no loop-based auto-advance
		// JSON `hold`: freeze the stage's clips on ONE frame instead of playing them. The value is a
		// normalized clip position (`true` = 1.0 = the last frame). < 0 = not frozen (the default).
		// A frozen stage never loops, so only `timer` or a manual advance leaves it.
		float                    hold = -1.0f;
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
		std::string name;  // "" = an anonymous positional role (unified *.osf.json; clips index-align to role order)
		RoleGender  gender = RoleGender::kAny;
		// Role filters, VALIDATED at scene load via the form-ref resolver but stored as FormIDs:
		// Starfield reference-counts forms (TESFormRefCount) and destroys ones nothing loaded holds,
		// so a raw TESForm* cached across frames can dangle — re-resolve with LookupByID at use
		// (which reloads an unloaded form on demand).
		// The role's bound actor must satisfy every PRESENT constraint;
		// within `keywords`/`races` it is any-of (the actor needs ANY listed keyword, and ANY listed race).
		// An empty vector = that constraint is absent. `gender` desugars from `gender`/`filters.gender`.
		std::vector<RE::TESFormID> keywords;  // BGSKeyword ids; empty = no keyword constraint
		std::vector<RE::TESFormID> races;     // TESRace ids; empty = no race constraint
		std::vector<std::string>   preserveBones;  // exact, case-insensitive rig names left engine-driven
		std::string                mask;  // canonical named driven-bone mask ("upperBody"...); "" = every body bone binds
		Animation::PoseMode        poseMode = Animation::PoseMode::kOverride;  // absolute replacement (default) or rest-relative layer
		float                      poseWeight = 1.0f;  // persistent role layer strength, normalized to [0,1]
		Animation::ParticipantPlacement offset{};  // default placement for this role
		RoleEquip                    equip;     // optional per-gender item equipped for the scene's duration
	};

	// Per-scene OSF control grant. This is distinct from `lockPlayer`, which suppresses vanilla
	// movement/combat input. The legacy JSON key remains `playerControl` for compatibility.
	struct SceneControls
	{
		bool          enabled = true;                  // false => no input channel at all
		std::uint32_t capabilities = Input::kAllCapabilities;  // capabilities granted (default: all; `disable` removes)
		bool          locked = false;                  // player may not end the scene via the input channel (story scenes)
	};
	using PlayerControl = SceneControls;  // legacy domain spelling

	enum class CatalogSourceKind : std::uint8_t
	{
		kAuthoredScene,
		kCuratedAnimation,
		kDerivedDebugAnimation,
		kReferenceAnimation
	};

	inline constexpr std::string_view CatalogSourceKindName(CatalogSourceKind a_kind) noexcept
	{
		switch (a_kind) {
		case CatalogSourceKind::kAuthoredScene: return "authoredScene";
		case CatalogSourceKind::kCuratedAnimation: return "curatedAnimation";
		case CatalogSourceKind::kDerivedDebugAnimation: return "derivedDebugAnimation";
		case CatalogSourceKind::kReferenceAnimation: return "referenceAnimation";
		}
		return "authoredScene";
	}

	struct SceneDef
	{
		std::string              id;
		std::string              name;

		// Skeleton family the scenes clips target (Util::SpeciesFromAnimPath of the first clip): "human" (default / no creature clip), "terrormorph", etc...
		std::string              species;
		std::int32_t             priority = 0;
		std::int32_t             weight = 1;  // weighted-random sampling within the top priority tier (StartSceneByTags*)
		bool                     unlisted = false;  // excluded from the matchmaking pool; only reachable by direct id
		// Generated one-clip entry built from an EXPLICIT `clipLibrary` registration rather than
		// harvested from a scene's stages. Both share the `osf.scene-clip/` id namespace, but only
		// the harvested ones are a debug surface: a registration is authored content a pack shipped
		// on purpose, so the browser must show it to everyone. Nothing else can tell them apart —
		// an author may register a clip with no tags, name, or folder at all.
		bool                     curatedClip = false;
		bool                     library = false;   // file-level `section:"library"`: reference-library lane (osf.library.data), kept out of the main catalog
		CatalogSourceKind        sourceKind = CatalogSourceKind::kAuthoredScene;
		bool                     clipsAvailable = true;  // false: a referenced clip resolves to no installed file (compat pack without its source mod) — hidden from the catalog and matchmaking; a direct-id start still attempts and logs the load failure
		bool                     playerInputLock = true;  // legacy JSON `lockPlayer`
		bool                     hideApparel = true;      // legacy JSON `stripActors`; base skin kept
		bool                     clearHeldItems = true;  // Unequip equipped non-apparel at scene start, auto-restored on end;
		bool                     fade = false;  // Screen fade-to-black on start when the player participates (self-releasing curtain); OFF by default, opt in with `fade:true`
		Animation::WorldPlacementMode worldPlacement = Animation::WorldPlacementMode::kAnchorAndPin;  // legacy JSON `inPlace`
		SceneControls            sceneControls;  // legacy JSON `playerControl`
		std::vector<std::string> tags;
		// Lowercased `tags`, built once at parse: matchmaking tag-matches EVERY loaded def per
		// query, so the per-def lowering/set-build must not happen on that path.
		std::unordered_set<std::string> tagSet;
		std::vector<SceneRole>   roles;
		std::string              entry;
		std::vector<SceneNode>   nodes;
		std::vector<std::string> linearStages;  // optional: stage i -> node id (GetSceneStage/SetSceneStage)

		std::filesystem::path    sourceFile;
		std::string              pack;  // optional file-level `pack`: the content-pack display name the browser groups this scene under (a pack may span many files); "" = derive from sourceFile
		std::string              folder;  // optional slash-delimited catalog path within the pack; presentation only

		// When anchorKeywords/anchorBaseForms set, the scene is ANCHOR-BOUND. can only start anchored to ref with base form OR has keyword.
		// Both stored as FormIDs, validated at load, re-resolved via LookupByID at use — never cache the
		// TESForm* itself: Starfield refcounts forms and destroys ones nothing loaded references (a vanilla
		// furniture keyword can die while the player is off-world), so a session-cached pointer dangles.
		std::vector<RE::TESFormID>      anchorKeywords;   // BGSKeyword ids (any-of); empty = no keyword match
		std::vector<RE::TESFormID>      anchorBaseForms;  // base-form ids (any-of); empty = no base match
		Animation::ParticipantPlacement anchorOffset{};

		[[nodiscard]] bool RequiresAnchor() const noexcept { return !anchorKeywords.empty() || !anchorBaseForms.empty(); }

		const SceneNode* FindNode(std::string_view a_id) const;

		// Index of a_nodeId in linearStages (case-insensitive), or -1 (also -1 if the scene declares no linearStages — a non-linear graph has no stage number).
		std::int32_t LinearStageOf(std::string_view a_nodeId) const;
	};

	// Persistent one-actor overlay route definitions. Routes deliberately carry no scene-policy
	// fields: they only describe masked local-pose layers and externally-observable transition marks.
	enum class RouteLifetime : std::uint8_t
	{
		kTransition,
		kStation,
		kController,
		kExternal
	};

	enum class RouteInterruption : std::uint8_t
	{
		kFinish,
		kCrossfadeBeforeCommit
	};

	struct RouteLayer
	{
		StageClip             clip;
		std::string           mask;       // mandatory named BoneMask
		Animation::PoseMode   mode = Animation::PoseMode::kOverride;
		float                 weight = 1.0f;
		float                 holdAt = -1.0f;  // station only; normalized pose position
	};

	struct RouteMarker
	{
		float       frame = 0.0f;
		std::string id;
	};

	struct RouteProp
	{
		float             frame = 0.0f;
		std::string       id;
		bool              attach = true;
		RouteLifetime     lifetime = RouteLifetime::kTransition;
		Props::Source     source;
		Props::Attachment attachment;
	};

	struct RouteSound
	{
		float       frame = 0.0f;
		std::string spec;
	};

	struct RouteStation
	{
		std::string               id;
		std::optional<RouteLayer> layer;  // absent = zero-animation station
	};

	struct RouteTransition
	{
		std::string                 id;
		std::string                 from;
		std::string                 to;
		RouteLayer                  layer;
		std::optional<RouteMarker>  commit;
		std::vector<RouteMarker>    markers;
		std::vector<RouteProp>      props;
		std::vector<RouteSound>     sounds;
		RouteInterruption           interruption = RouteInterruption::kFinish;
		std::optional<Animation::ContactPose> contactPose;
	};

	struct RouteDef
	{
		std::string                  id;
		std::vector<RouteStation>    stations;
		std::vector<RouteTransition> transitions;
		std::filesystem::path        sourceFile;

		const RouteStation* FindStation(std::string_view a_id) const
		{
			const auto equal = [](std::string_view a_lhs, std::string_view a_rhs) {
				return a_lhs.size() == a_rhs.size() && std::equal(a_lhs.begin(), a_lhs.end(), a_rhs.begin(),
					[](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
			};
			const auto it = std::find_if(stations.begin(), stations.end(), [&](const RouteStation& a_station) { return equal(a_station.id, a_id); });
			return it == stations.end() ? nullptr : &*it;
		}
		const RouteTransition* FindTransition(std::string_view a_id) const
		{
			const auto equal = [](std::string_view a_lhs, std::string_view a_rhs) {
				return a_lhs.size() == a_rhs.size() && std::equal(a_lhs.begin(), a_lhs.end(), a_rhs.begin(),
					[](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
			};
			const auto it = std::find_if(transitions.begin(), transitions.end(), [&](const RouteTransition& a_transition) { return equal(a_transition.id, a_id); });
			return it == transitions.end() ? nullptr : &*it;
		}
	};

	// A structured view of one legacy load-error line. `message` remains the exact text published
	// through OSFAdvanced.GetSceneLoadErrors(); the remaining fields let the browser explain and group it
	// without reverse-engineering prose. Fields are additive and may be empty when not applicable.
	struct SceneImportProblem
	{
		bool        warning = false;
		std::string code;
		std::string message;
		std::string hint;
		std::string scene;
		std::string node;
		std::string role;
		std::string clip;
	};
	// Preferred content-wide spelling. The legacy name remains the underlying type so existing
	// source and binary identities do not change while callers migrate.
	using ContentImportProblem = SceneImportProblem;

	// What ONE *.osf.json contributed to the registry: the author-facing import record behind the
	// browser's IMPORTS panel. Every discovered file gets one, including a file that was rejected
	// whole and contributed nothing — "my pack is missing" is answered by the row being present with
	// zero scenes and its reject line attached, which a scene-only view can never show.
	// Aggregated from the final published content, so the counts are what actually reached the registry.
	struct SceneFileStats
	{
		// Never an absolute path: it names the player's machine and account. `path` is relative to
		// Data/OSF, which is what actually disambiguates two packs both shipping "scenes.osf.json".
		std::string   file;              // file name only
		std::string   path;              // slash-delimited, relative to Data/OSF ("" = the cross-file bucket)
		std::string   pack;              // file-level `pack` label ("" = none authored)
		bool          library = false;   // file-level section:"library" (reference lane)
		std::int64_t  schema = 0;        // declared `schema` (0 = absent, unreadable, or the file failed to parse)
		std::uint64_t bytes = 0;         // size on disk
		float         parseMs = 0.0f;    // read + parse + validate wall time for this file

		std::uint32_t scenes = 0;        // scenes accepted into the registry
		std::uint32_t declaredScenes = 0; // scene objects authored in this file (before validation)
		std::uint32_t hidden = 0;        //   ...of those, hidden by the availability sweep (!clipsAvailable)
		std::uint32_t rejectedScenes = 0; // declaredScenes - scenes (whole-file rejection included)
		std::uint32_t routes = 0;         // routes accepted into the registry
		std::uint32_t declaredRoutes = 0; // route objects authored in this file
		std::uint32_t rejectedRoutes = 0; // declaredRoutes - routes
		std::uint32_t unlisted = 0;      //   ...of those, out of the matchmaking pool (direct id only)
		std::uint32_t anchored = 0;      //   ...of those, anchor-bound (furniture/marker required)
		std::uint32_t nodes = 0;
		std::uint32_t stages = 0;
		std::uint32_t roles = 0;
		std::uint32_t clips = 0;          // clip slots (stage x role) — what playback will actually load
		std::uint32_t distinctClips = 0;  // distinct clip specs referenced
		std::uint32_t missingClips = 0;   //   ...of those, ones that resolve to no installed file
		std::uint32_t cues = 0;           // track-lane entries, summed over every accepted node
		std::uint32_t actions = 0;
		std::uint32_t sounds = 0;
		std::uint32_t cameras = 0;
		std::uint32_t clipEntries = 0;    // generated one-clip library entries sourced from this file
		std::vector<std::string> species;  // distinct skeleton families, sorted

		std::vector<std::string> missingClipExamples;  // bounded, deterministic examples for repair UI
		std::uint32_t errors = 0;
		std::uint32_t warnings = 0;
		std::vector<ContentImportProblem> problems;  // full structured set, in legacy load-error order

		[[nodiscard]] bool Rejected() const noexcept { return scenes == 0 && routes == 0 && clipEntries == 0 && errors > 0; }
	};
	// Preferred content-wide spelling; SceneFileStats remains available for compatibility.
	using ContentFileStats = SceneFileStats;

	// Immutable publication unit. Reload builds one privately and atomically replaces the current
	// pointer; readers and live scenes retain shared ownership of the exact definitions they use.
	struct SceneRegistrySnapshot
	{
		std::unordered_map<std::string, SceneDef> scenes;
		std::unordered_map<std::string, RouteDef> routes;
		std::vector<std::string> loadErrors;
		size_t authoredSceneCount = 0;  // excludes generated one-clip browser/debug entries
		// One record per discovered *.osf.json, sorted by `path`. A trailing record with an empty
		// `path` collects problems no single file owns (there normally are none).
		std::vector<ContentFileStats> files;
	};
	// Preferred content-wide spelling; SceneRegistrySnapshot remains available for compatibility.
	using ContentRegistrySnapshot = SceneRegistrySnapshot;

	template <class T>
	class RegistryRef
	{
	public:
		RegistryRef() = default;
		RegistryRef(const RegistryRef&) = default;
		RegistryRef& operator=(const RegistryRef&) = default;
		RegistryRef(RegistryRef&& a_other) noexcept :
			owner(std::move(a_other.owner)), value(std::exchange(a_other.value, nullptr))
		{}
		RegistryRef& operator=(RegistryRef&& a_other) noexcept
		{
			if (this != &a_other) {
				owner = std::move(a_other.owner);
				value = std::exchange(a_other.value, nullptr);
			}
			return *this;
		}
		[[nodiscard]] explicit operator bool() const noexcept { return value != nullptr; }
		[[nodiscard]] const T* get() const noexcept { return value; }
		[[nodiscard]] const T* operator->() const noexcept { return value; }
		[[nodiscard]] const T& operator*() const noexcept { return *value; }

	private:
		friend class SceneRegistry;
		std::shared_ptr<const ContentRegistrySnapshot> owner;
		const T* value = nullptr;
	};

	using RouteRef = RegistryRef<RouteDef>;
	using SceneRef = RegistryRef<SceneDef>;

	class SceneRegistry
	{
	public:
		static SceneRegistry& GetSingleton();

		// Scans Data/OSF/**/*.osf.json and rebuilds the registry.
		// Bad scenes are skipped; every skip and warning is both logged and recorded for LoadErrors().
		// Runs at startup and again on OSF.ReloadPacks().
		void LoadAll();

		// Scene by id (case-insensitive). The returned ref pins the immutable registry snapshot,
		// so its definition remains valid across ReloadPacks and can be retained by a live scene.
		SceneRef Find(std::string_view a_id) const;

		// Overlay route by id (case-insensitive), with independent snapshot lifetime pinning.
		RouteRef FindRoute(std::string_view a_id) const;

		// Resolve a flow node's inline authored `stages`, or a `use` target, to a Layer-A PlaybackPlan.
		// a_actorCount must equal the resolved role count.
		std::optional<Animation::PlaybackPlan> BuildNodePlan(const SceneRef& a_def, const SceneNode& a_node, size_t a_actorCount) const;

		// Visit every definition in one pinned snapshot. A concurrent reload publishes a new
		// snapshot without invalidating this iteration.
		void ForEachDef(const std::function<void(const SceneDef&)>& a_fn) const;

		// Visit every overlay route in one pinned snapshot. Used by author tooling that needs
		// the route topology without borrowing pointers across a pack reload.
		void ForEachRoute(const std::function<void(const RouteDef&)>& a_fn) const;

		size_t Size() const;

		// Problems (errors + warnings) from the last LoadAll, for OSFAdvanced.GetSceneLoadErrors().
		std::vector<std::string> LoadErrors() const;

		// Per-content-file import records from the last LoadAll, sorted by path (see ContentFileStats).
		std::vector<ContentFileStats> FileStats() const;

		// Data-relative clip references from loaded scenes whose resolved file does not currently exist.
		std::vector<std::string> MissingClipRefs() const;

	private:
		std::atomic<std::shared_ptr<const ContentRegistrySnapshot>> snapshot{
			std::make_shared<const ContentRegistrySnapshot>()
		};
	};

	using ContentRegistry = SceneRegistry;  // preferred content-wide spelling
}
