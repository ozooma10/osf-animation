#pragma once

// Loads JSON animation packs from Data/OSF/** and resolves animation ids to per-actor files
// and placements. It only reads the mechanical structure (id, tags, gender slots, stages,
// clips, offsets, timer, loops); any content fields a pack carries are ignored here. Offsets
// are alignment corrections.

#include "Animation/Scene.h"

namespace OSF::Registry
{
	// Highest pack schema version we understand. We read the stage-major layout (version 2);
	// older actor-major (version 1) packs are rejected with a migration hint in ParseAnimation.
	// Bump only on a breaking change.
	inline constexpr std::int64_t kPackSchemaVersion = 2;

	enum class SlotGender : std::uint8_t
	{
		kAny,
		kMale,
		kFemale
	};

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
		float timer = 0.0f;            // seconds; 0 = no auto-advance
		int32_t loops = 0;            // clip loops; 0 = no auto-advance
		std::vector<StageClip> clips;  // one per actor, index-aligned with AnimationDef::actors
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
	};

	class PackRegistry
	{
	public:
		static PackRegistry& GetSingleton();

		// Scans Data/OSF/**/*.json and rebuilds the registry. Bad files or entries are logged
		// and skipped. Runs at startup and again on OSF.ReloadPacks().
		void LoadAll();

		// Resolves id -> a multi-stage ScenePlan (files + placements + timer/loops), or nullopt (reason logged).
		std::optional<Animation::ScenePlan> BuildScenePlan(std::string_view a_id, size_t a_actorCount) const;

		// Ids with a_actorCount actors whose tags contain ALL a_tags (case-insensitive; empty = any). Sorted for determinism.
		std::vector<std::string> FindByTags(size_t a_actorCount, const std::vector<std::string>& a_tags) const;

		// order[slot] = index into the caller's actor list filling that definition slot.
		struct SlottedPick
		{
			std::string id;
			std::vector<size_t> order;
		};

		// Random animation matching a_genders.size() actors + a_tags whose gender slots these actors can fill (kAny accepts anyone; kNone fits only kAny).
		std::optional<SlottedPick> PickByTags(const std::vector<std::string>& a_tags, const std::vector<RE::SEX>& a_genders) const;

		size_t Size() const;

	private:
		mutable std::shared_mutex lock;
		std::unordered_map<std::string, AnimationDef> animations;  // key = lowercased id
	};
}
