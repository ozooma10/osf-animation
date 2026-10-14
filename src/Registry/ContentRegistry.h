#pragma once

// Loads scenes from Data/OSF/**/*.osf.json; `clip`/`stages[]` shorthand desugars to a node chain.

#include "Animation/Scene.h"
#include "Input/InputTypes.h"
#include "Props/PropTypes.h"
#include "API/OSFClipScene.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace OSF::Registry
{
	inline constexpr std::int64_t kSchemaVersion = 1;

	enum class RoleGender : std::uint8_t
	{
		kAny,
		kMale,
		kFemale
	};

	// "male"/"m", "female"/"f" (case-insensitive); else kAny.
	RoleGender ParseRoleGender(std::string_view a_str);

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
		std::string  labelKey;
		std::string  to;        // target node id, or "$end"
		EdgeWhen     when = EdgeWhen::kAdvance;
		std::string  trigger;   // cue id (when == kTrigger)
		bool         isDefault = false;
		std::int32_t priority = 0;
	};

	// Shared position model for all four node tracks.
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

	// `atFrame` rate: Creation Engine 30 fps (AFImport::kAfFps); frame N = N / 30 clip-local seconds.
	inline constexpr float kFrameRate = 30.0f;

	enum class SoundEmitter : std::uint8_t
	{
		kListener,
		kRole
	};

	// Fires EVENT_CUE.
	struct CueEntry
	{
		CuePos       pos = CuePos::kEnter;
		float        fraction = 0.0f;   // when pos == kFraction and frame < 0
		float        frame = -1.0f;     // authored `atFrame`; < 0 = authored as a fraction
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

	// `osf.*` types are built-in; any other namespace is custom, emitted as EVENT_ACTION.
	struct ActionEntry
	{
		ActionPos    pos = ActionPos::kEnter;
		float        fraction = 0.0f;    // when pos == kFraction and frame < 0
		float        frame = -1.0f;      // authored `atFrame`; < 0 = authored as a fraction
		bool         everyLoop = false;  // repeat:"loop" (numeric only)
		std::string  type;
		ActionKind   kind = ActionKind::kCustom;
		std::string  role;
		bool         hold = false;       // osf.fade.out: end faded (opt out of the cleanup fade-in)
		float        duration = 0.0f;    // osf.fade.*: ramp seconds (0 = mechanism default)
		std::string  set;    // osf.voice.play: Data-relative path or "event:<name>"
		SoundEmitter emitter = SoundEmitter::kListener;  // osf.voice.play
		std::string  item;   // osf.equipment.equip: "<plugin>|0xLOCAL"
		std::string  prop;    // osf.prop.*: scene-local prop id
		Props::Source     propSource;      // osf.prop.attach: fixed form or equipped-armor selector
		Props::Attachment propAttachment;  // osf.prop.attach: actor node + local transform
	};

	// `spec`: Data-relative path, "event:<name>"/"event:0x<id>", or a '$tag,tag' SoundRegistry
	// pool query re-rolled on every fire.
	struct SoundEntry
	{
		SoundPos     pos = SoundPos::kFraction;
		float        fraction = 0.0f;
		float        frame = -1.0f;  // authored `atFrame`; < 0 = authored as a fraction
		bool         everyLoop = false;
		std::string  spec;
		std::string  role;    // optional voice channel, gender source, and subtitle speaker
		SoundEmitter emitter = SoundEmitter::kListener;
	};

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

	// Held camera state, auto-restored on cleanup. A file-level `camera` default lands on the entry node.
	struct CameraEntry
	{
		CameraPos    pos = CameraPos::kEnter;
		float        fraction = 0.0f;
		float        frame = -1.0f;  // authored `atFrame`; < 0 = authored as a fraction
		bool         everyLoop = false;
		CameraState  state = CameraState::kNone;
		float        distance = 0.0f;  // thirdperson_hold opening pull-back; 0 = engine default
	};

	// Clip-local seconds for an `atFrame` entry; -1 for a fraction `at`.
	template <class Entry>
	constexpr float TrackSeconds(const Entry& a_entry)
	{
		return a_entry.frame >= 0.0f ? a_entry.frame / kFrameRate : -1.0f;
	}

	// With no known duration only frame 0 is placeable; later frames report the clip end.
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

	// PlaybackSession::Advance fires marks in a [prev, next) window capped at the clip duration, so an `atFrame`
	// at/past the end never fires even though TrackFraction clamps it to 1.0. Unknowns return true.
	template <class Entry>
	inline bool TrackFires(const Entry& a_entry, float a_durationSec)
	{
		const float sec = TrackSeconds(a_entry);
		return sec < 0.0f || a_durationSec <= 0.0f || sec < a_durationSec;
	}

	struct StageClip
	{
		std::string file;
		std::string animId;
		float       sec = 0.0f;  // pack-authored duration; 0 = unknown, probe at runtime
		std::optional<Animation::ParticipantPlacement> offset;  // overrides the role's default placement
		std::optional<std::string> mask;  // overrides the role's bone mask for this stage
	};

	// timer/loops 0 = hold; a stage authoring neither defaults to loops=1 at parse.
	struct StageDef
	{
		std::string              name;          // browser label for this stage's animation ("" = none)
		std::vector<std::string> tags;
		float                    timer = 0.0f;  // seconds; 0 = none
		std::int32_t             loops = 0;     // 0 = none
		// JSON `hold`: freeze at a normalized clip position (`true` = 1.0); < 0 = not frozen. Never loops.
		float                    hold = -1.0f;
		std::vector<StageClip>   clips;         // one per role, role order
		// DesugarLinear forwards these onto the stage's synthetic node.
		std::vector<CueEntry>    cues;
		std::vector<ActionEntry> actions;
		std::vector<SoundEntry>  sounds;
		std::vector<CameraEntry> cameras;
	};

	struct SceneNode
	{
		std::string              id;
		// Exactly one of `use` (scene id) or `stages` (inline timeline).
		std::string              use;
		std::vector<StageDef>    stages;
		LoopMode                 loopMode = LoopMode::kOnce;  // JSON `loops`: omit=once, 0=hold, N=count
		std::int32_t             loopCount = 0;              // when loopMode == kCount
		float                    timerSec = 0.0f;            // JSON `timer` (seconds)
		std::vector<SceneEdge>   edges;
		std::vector<CueEntry>    cues;
		std::vector<ActionEntry> actions;
		std::vector<SoundEntry>  sounds;
		std::vector<CameraEntry> cameras;
	};

	// Equipped at scene start, removed on every end path (kEquipItem ledger). "<Plugin>|0xLOCAL" refs
	// resolved at fire time; an uninstalled plugin warns + skips rather than rejecting the scene.
	struct RoleEquip
	{
		std::string male;
		std::string female;
		std::string any;  // fallback; also the bare-string form

		[[nodiscard]] bool Empty() const noexcept { return male.empty() && female.empty() && any.empty(); }

		// May return "" (caller skips).
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
		std::string name;  // "" = anonymous positional role; clips index-align to role order
		RoleGender  gender = RoleGender::kAny;
		// FormIDs, not TESForm*: forms are refcounted and destroyed when unheld, so re-resolve via LookupByID.
		// Every non-empty constraint must hold; each list is any-of.
		std::vector<RE::TESFormID> keywords;  // BGSKeyword ids
		std::vector<RE::TESFormID> races;     // TESRace ids
		std::vector<std::string>   preserveBones;  // exact, case-insensitive rig names left engine-driven
		std::string                mask;  // canonical named driven-bone mask ("upperBody"...); "" = every body bone binds
		Animation::PoseMode        poseMode = Animation::PoseMode::kOverride;
		float                      poseWeight = 1.0f;  // [0,1]
		Animation::ParticipantPlacement offset{};
		RoleEquip                    equip;
	};

	// OSF input-channel grant (JSON `playerControl`); distinct from `lockPlayer`, which blocks vanilla input.
	struct SceneControls
	{
		bool          enabled = true;                  // false = no input channel
		std::uint32_t capabilities = Input::kAllCapabilities;  // `disable` removes
		bool          locked = false;                  // player can't end the scene via the channel
	};

	enum class CatalogSourceKind : std::uint8_t
	{
		kAuthoredScene,
		kCuratedAnimation,
		kProvidedScene,
		kReferenceAnimation
	};

	inline constexpr std::string_view CatalogSourceKindName(CatalogSourceKind a_kind) noexcept
	{
		switch (a_kind) {
		case CatalogSourceKind::kAuthoredScene: return "authoredScene";
		case CatalogSourceKind::kCuratedAnimation: return "curatedAnimation";
		case CatalogSourceKind::kProvidedScene: return "providedScene";
		case CatalogSourceKind::kReferenceAnimation: return "referenceAnimation";
		}
		return "authoredScene";
	}

	struct SceneDef
	{
		std::string              provider;  // optional native content-provider owner
		std::string              id;
		std::string              name;

		// Util::SpeciesFromAnimPath of the first clip; "human" when no creature clip.
		std::string              species;
		std::int32_t             priority = 0;
		std::int32_t             weight = 1;  // weighted-random within the top priority tier
		bool                     unlisted = false;  // not matchmade; direct id only
		// From an explicit `clipLibrary` registration.
		bool                     curatedClip = false;
		bool                     library = false;   // file-level `section:"library"`; kept out of the main catalog
		CatalogSourceKind        sourceKind = CatalogSourceKind::kAuthoredScene;
		bool                     clipsAvailable = true;  // false: a clip isn't installed; hidden from catalog/matchmaking, direct id still attempts
		bool                     playerInputLock = true;  // JSON `lockPlayer`
		bool                     hideApparel = true;      // JSON `stripActors`; base skin kept
		bool                     clearHeldItems = true;  // non-apparel; restored on end
		bool                     fade = false;  // fade-to-black on start when the player participates
		Animation::WorldPlacementMode worldPlacement = Animation::WorldPlacementMode::kAnchorAndPin;  // JSON `inPlace`
		SceneControls            sceneControls;  // JSON `playerControl`
		std::vector<std::string> tags;
		// Lowercased `tags`, prebuilt: matchmaking scans every def per query.
		std::unordered_set<std::string> tagSet;
		std::vector<SceneRole>   roles;
		std::string              entry;
		std::vector<SceneNode>   nodes;
		std::vector<std::string> linearStages;  // optional: stage i -> node id (GetSceneStage/SetSceneStage)

		std::filesystem::path    sourceFile;
		std::string              pack;  // browser group name; "" = derive from sourceFile
		std::string              folder;  // slash-delimited catalog path within the pack

		// Non-empty = anchor-bound: starts only on a ref matching a base form or keyword (any-of).
		// FormIDs re-resolved at use; a cached TESForm* can dangle (forms are destroyed when unheld).
		std::vector<RE::TESFormID>      anchorKeywords;   // BGSKeyword ids
		std::vector<RE::TESFormID>      anchorBaseForms;
		Animation::ParticipantPlacement anchorOffset{};

		[[nodiscard]] bool RequiresAnchor() const noexcept { return !anchorKeywords.empty() || !anchorBaseForms.empty(); }

		const SceneNode* FindNode(std::string_view a_id) const;

		// Case-insensitive; -1 if absent or the scene has no linearStages.
		std::int32_t LinearStageOf(std::string_view a_nodeId) const;
	};

	// `message` is the exact OSFAdvanced.GetSceneLoadErrors() line; other fields may be empty.
	struct ContentImportProblem
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

	// Import record per discovered *.osf.json, including files rejected whole (they show as zero-scene
	// rows). Counts reflect the final published content.
	struct ContentFileStats
	{
		// Never absolute (would expose the user's machine/account).
		std::string   file;              // file name only
		std::string   path;              // slash-delimited, relative to Data/OSF ("" = the cross-file bucket)
		std::string   pack;              // "" = none authored
		bool          library = false;
		std::int64_t  schema = 0;        // 0 = absent, unreadable, or the file failed to parse
		std::uint64_t bytes = 0;
		float         parseMs = 0.0f;    // read + parse + validate wall time

		std::uint32_t scenes = 0;        // accepted
		std::uint32_t declaredScenes = 0; // authored, before validation
		std::uint32_t hidden = 0;        //   ...of scenes, !clipsAvailable
		std::uint32_t rejectedScenes = 0; // declaredScenes - scenes
		std::uint32_t unlisted = 0;
		std::uint32_t anchored = 0;
		std::uint32_t nodes = 0;
		std::uint32_t stages = 0;
		std::uint32_t roles = 0;
		std::uint32_t clips = 0;          // clip slots (stage x role)
		std::uint32_t distinctClips = 0;
		std::uint32_t missingClips = 0;   //   ...of distinctClips
		std::uint32_t cues = 0;           // summed over accepted nodes
		std::uint32_t actions = 0;
		std::uint32_t sounds = 0;
		std::uint32_t cameras = 0;
		std::uint32_t clipEntries = 0;    // generated one-clip entries sourced from this file
		std::vector<std::string> species;

		std::vector<std::string> missingClipExamples;  // bounded, deterministic
		std::uint32_t errors = 0;
		std::uint32_t warnings = 0;
		std::vector<ContentImportProblem> problems;  // in load-error order

		[[nodiscard]] bool Rejected() const noexcept { return scenes == 0 && clipEntries == 0 && errors > 0; }
	};

	// Immutable; reload builds a new one and atomically swaps the pointer. Readers hold shared ownership.
	struct ContentRegistrySnapshot
	{
		std::unordered_map<std::string, bool> clipAvailability;
		std::unordered_map<std::string, SceneDef> scenes;
		std::vector<std::string> loadErrors;
		size_t authoredSceneCount = 0;  // excludes generated one-clip entries
		// Sorted by `path`; a trailing empty-`path` record holds cross-file problems.
		std::vector<ContentFileStats> files;
	};

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
		friend class ContentRegistry;
		std::shared_ptr<const ContentRegistrySnapshot> owner;
		const T* value = nullptr;
	};

	using SceneRef = RegistryRef<SceneDef>;

	class ContentRegistry
	{
	public:
		static ContentRegistry& GetSingleton();

		// Pins one publication for multi-part public metadata reads during concurrent reloads.
		std::shared_ptr<const ContentRegistrySnapshot> ReadSnapshot() const { return snapshot.load(); }

		// Bad scenes are skipped, logged, and recorded for LoadErrors(). Also runs on OSF.ReloadPacks().
		void LoadAll();
		using ClipProvider = std::function<std::vector<API::ClipScene>(const ContentRegistrySnapshot&)>;
		// Providers run synchronously under the publication lock; they must not reenter the registry.
		bool RegisterClipProvider(std::string a_owner, ClipProvider a_provider);
		bool UnregisterClipProvider(std::string_view a_owner);

		// Case-insensitive. The ref pins the snapshot, so it stays valid across ReloadPacks.
		SceneRef Find(std::string_view a_id) const;

		// Resolves inline `stages` or a `use` target; a_actorCount must equal the resolved role count.
		std::optional<Animation::PlaybackPlan> BuildNodePlan(const SceneRef& a_def, const SceneNode& a_node, size_t a_actorCount) const;

		// Iterates one pinned snapshot; safe across a concurrent reload.
		void ForEachDef(const std::function<void(const SceneDef&)>& a_fn) const;

		size_t Size() const;

		std::vector<std::string> LoadErrors() const;
		std::vector<ContentFileStats> FileStats() const;

		// Data-relative clip refs whose resolved file doesn't exist.
		std::vector<std::string> MissingClipRefs() const;

	private:
		void PublishProviders();  // providerMutex held; readers keep the old publication until complete
		std::mutex providerMutex;
		std::map<std::string, ClipProvider> providers;
		std::shared_ptr<const ContentRegistrySnapshot> baseSnapshot{std::make_shared<const ContentRegistrySnapshot>()};
		std::atomic<std::shared_ptr<const ContentRegistrySnapshot>> snapshot{
			std::make_shared<const ContentRegistrySnapshot>()
		};
	};

}
