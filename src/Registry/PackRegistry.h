#pragma once

// Animation/clip pack registry: loads SLAL-shaped JSON animation packs from
// Data/OSF/**, then resolves animation ids to per-actor files + placements for
// playback. This is the content-neutral CORE registry (OSF Animation): it
// parses the clip/stage/tag/gender-slot/offset structure and ignores the OSF
// content fields (undress/equipment, scheduled voice, stage intensity/peak,
// content cues) — those belong to the OSF Intimacy scene engine.
//
// CANONICAL SCHEMA DOC: docs/PACK_SCHEMA.md — the modder-facing contract this
// parser implements. Any change to parsing behavior updates that file in the
// same commit.
//
// The schema deliberately mirrors SLAL (SexLab Animation Loader) concepts —
// pack -> animations -> actors -> stages, plus tags and per-stage offsets —
// so a future converter for the Skyrim SexLab animation library (offline
// HKX -> retarget -> GLB) can carry its metadata across mechanically.
// SexLab animations are authored co-located around a shared origin, which is
// exactly this framework's anchor model; offsets are alignment corrections.

#include "Animation/Scene.h"

namespace OSF::Registry
{
	// Highest pack "schema" version this build can parse. Bump on any change
	// that would make an older parser misread a pack (not on additive fields —
	// unknown fields are ignored).
	// v2 = stage-major layout (top-level stages[].clips[]); the v1 actor-major
	// layout (actors[].stages[]) was removed and is now rejected with a migration
	// hint. A v2 pack on a v1 build is skipped cleanly with "update OSF Animation".
	inline constexpr std::int64_t kPackSchemaVersion = 2;

	enum class SlotGender : std::uint8_t
	{
		kAny,
		kMale,
		kFemale
	};

	// One actor's clip for one stage. The per-stage `clips[]` array holds one of
	// these per actor slot, in actor order.
	struct StageClip
	{
		std::string file;
		std::optional<Animation::ParticipantPlacement> offset;  // overrides the actor default
	};

	// Stage-invariant per-actor metadata: one entry per slot, no per-stage data.
	// The clips an actor plays live in StageDef::clips, index-aligned with this.
	struct SlotDef
	{
		SlotGender gender = SlotGender::kAny;      // who can fill this slot
		Animation::ParticipantPlacement offset{};  // default placement for all stages
	};

	// One stage of the scene timeline: timing plus one clip per actor.
	struct StageDef
	{
		float timer = 0.0f;  // seconds; 0 = no auto-advance
		int32_t loops = 0;   // completed clip loops; 0 = no auto-advance
		std::vector<StageClip> clips;  // one per actor slot, index-aligned with AnimationDef::actors
	};

	struct AnimationDef
	{
		std::string id;
		std::string name;
		std::string pack;                   // source pack name, diagnostics only
		std::filesystem::path sourceFile;   // pack file this came from
		std::vector<std::string> tags;
		std::vector<SlotDef> actors;    // stage-invariant per-actor metadata, one per slot
		std::vector<StageDef> stages;   // the timeline; every stage has actors.size() clips
	};

	// Everything a play request needs for one animation at one stage.
	struct SceneSpec
	{
		std::vector<std::string> files;
		std::vector<Animation::ParticipantPlacement> placements;
	};

	class PackRegistry
	{
	public:
		static PackRegistry& GetSingleton();

		// Scans Data/OSF/**/*.json and replaces the registry contents. Bad files
		// and bad entries are logged and skipped, never fatal. Called at
		// kPostDataLoad and from the OSF.ReloadPacks() dev native.
		void LoadAll();

		// Resolves id (case-insensitive) at the given stage for the given party
		// size; returns files + placements, or nullopt with the reason logged.
		std::optional<SceneSpec> BuildSceneSpec(std::string_view a_id, size_t a_actorCount, int32_t a_stage) const;

		// Resolves id into a full multi-stage plan (files + placements + timer/
		// loop targets per stage) for PlaySceneStaged, or nullopt with the reason logged.
		std::optional<Animation::ScenePlan> BuildScenePlan(std::string_view a_id, size_t a_actorCount) const;

		// Ids of animations with the given actor count whose tags contain ALL
		// of a_tags (case-insensitive; empty = any). Sorted for determinism.
		std::vector<std::string> FindByTags(size_t a_actorCount, const std::vector<std::string>& a_tags) const;

		// A tag-query pick with slot assignment: order[slot] = index into the
		// caller's actor list of the actor filling that definition slot.
		struct SlottedPick
		{
			std::string id;
			std::vector<size_t> order;
		};

		// Picks a RANDOM animation matching a_genders.size() actors + a_tags
		// whose gender slots the given actors can fill (kAny slots accept
		// anyone; kNone actors only fit kAny slots). nullopt if none match.
		std::optional<SlottedPick> PickByTags(const std::vector<std::string>& a_tags, const std::vector<RE::SEX>& a_genders) const;

		size_t Size() const;

	private:
		mutable std::shared_mutex lock;
		std::unordered_map<std::string, AnimationDef> animations;  // key = lowercased id
	};
}
