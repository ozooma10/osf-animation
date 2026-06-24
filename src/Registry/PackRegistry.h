#pragma once

// Loads JSON animation packs from Data/OSF/** and resolves animation ids to per-actor files and placements. 
// It only reads the mechanical structure (id, tags, gender slots, stages, clips, offsets, timer, loops); any content fields a pack carries are ignored here. 
// Offsets are alignment corrections.

#include "Animation/Scene.h"

#include <functional>
#include <string_view>

namespace OSF::Registry
{
	// Highest pack schema version we understand. We read the stage-major layout (version 1).
	// Bump only on a breaking change.
	inline constexpr std::int64_t kPackSchemaVersion = 1;

	enum class SlotGender : std::uint8_t
	{
		kAny,
		kMale,
		kFemale
	};

	// Case-insensitive gender-string parse ("male"/"m" -> kMale, "female"/"f" -> kFemale, else kAny).
	// Shared by both registries: pack actor slots and scene role filters use the same vocabulary.
	SlotGender ParseSlotGender(std::string_view a_str);

	// One actor's clip for one stage (one per slot in StageDef::clips, actor order).
	struct StageClip
	{
		std::string file;
		std::optional<Animation::ParticipantPlacement> offset;  // overrides the actor default
	};

	// Stage-invariant per-actor metadata (clips live in StageDef::clips).
	struct SlotDef
	{
		SlotGender gender = SlotGender::kAny;       // who can fill this slot
		Animation::ParticipantPlacement offset{};  // default placement for all stages
	};

	struct StageDef
	{
		// These hold the engine-level values: 0 means "no auto-advance" for both (hold this stage forever).
		// The play-once convenience default, a stage that specifies NEITHER timer nor loops becomes loops=1
		float timer = 0.0f;            // seconds; 0 = no time-based auto-advance
		int32_t loops = 0;             // clip loops before advancing; 0 = no loop-based auto-advance
		std::vector<StageClip> clips;  // one per actor, index-aligned with AnimationDef::actors
	};

	// Resolved per-animation default-mechanism policy. Both mirror the scene-def opt-outs and are default-on: 
	// Each value is resolved at load, the pack's top-level setting supplies the default, which an individual animation may override.
	struct PackPolicy
	{
		bool stripActors = true;  // hide every participant's worn apparel on start (base skin kept)
		bool lockPlayer = true;   // disable player input while the player participates
	};

	struct AnimationDef
	{
		std::string id;
		std::string name;
		std::string pack;                  // source pack, diagnostics only
		std::filesystem::path sourceFile;
		std::vector<std::string> tags;
		std::vector<SlotDef> actors;
		std::vector<StageDef> stages;  // every stage has actors.size() clips
		PackPolicy policy;             // resolved opt-outs (pack top-level default, per-animation override)
	};

	class PackRegistry
	{
	public:
		static PackRegistry& GetSingleton();

		// Scans Data/OSF/**/*.json and rebuilds the registry. Bad files or entries are logged and skipped. 
		// Runs at startup and again on OSF.ReloadPacks().
		void LoadAll();

		// Resolves id -> a multi-stage ScenePlan (files + placements + timer/loops), or nullopt (reason logged).
		std::optional<Animation::ScenePlan> BuildScenePlan(std::string_view a_id, size_t a_actorCount) const;

		// Resolved default-mechanism policy for animation a_id, or the all-default policy if the id is unknow.
		PackPolicy GetPolicy(std::string_view a_id) const;

		// Tag/gender matchmaking moved to OSF::Matchmaking (src/Matchmaking/Matchmaker.*), which spans BOTH this registry and the scene registry; 
		// it reads packs via ForEachAnim below.

		// Visit every loaded animation def under the read lock (used by the matchmaker to build pack pseudo-candidates). 
		// The callback must NOT re-enter the registry (it holds the shared lock).
		void ForEachAnim(const std::function<void(const AnimationDef&)>& a_fn) const;

		size_t Size() const;

	private:
		mutable std::shared_mutex lock;
		std::unordered_map<std::string, AnimationDef> animations;  // key = lowercased id
	};
}
