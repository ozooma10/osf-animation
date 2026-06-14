#pragma once

// A synced multi-actor scene: one shared clock drives every participant graph
// so paired animations stay frame-locked regardless of job-thread ordering.
// Spatial layout: the scene owns the world anchor and per-participant
// ParticipantPlacement offsets; the BGSModelNode::Update compose-root pin
// applies anchor + rotated offset each frame to hold the rendered skeleton.
//
// Stages: every stage's clips are preloaded at scene start (no file IO on the
// job threads). Advance() auto-advances when the current stage's timer expires
// or its loop-count target is reached; each participant graph notices the
// stage change on its next sample and swaps its own animation (no cross-graph
// locking). After the last timed/loop-counted stage the scene flags `ended`
// and the update hook defers a StopScene to the game thread. Timer <= 0 and
// loops <= 0 = hold the stage until SetStage()/StopScene.
//
// This is the content-neutral core scene mechanism (OSF Animation). Scene
// policy — undress, scheduled voice, camera/control, fade choreography, the
// stall watchdog and Papyrus scene/cue callbacks — lives in the OSF Intimacy
// scene engine, not here.

#include "Animation/FrameClock.h"
#include "Animation/OzzTypes.h"

namespace OSF::Animation
{
	class Graph;

	struct ParticipantPlacement
	{
		// local offset from the scene anchor, rotated into the anchor's heading frame
		float x = 0.0f, y = 0.0f, z = 0.0f;
		float heading = 0.0f;  // radians, relative to anchor heading
	};

	// World position of a participant: the scene anchor plus the local (x, y, z)
	// offset rotated into the anchor's heading frame. The single definition of
	// how a placement maps to a world point — shared by the initial teleport and
	// the compose-root pin. (heading is applied separately, as a refr/anchor
	// rotation, not here.)
	inline RE::NiPoint3 PlacementToWorld(const RE::NiPoint3& a_anchorPos, float a_anchorHeading,
		const ParticipantPlacement& a_placement)
	{
		const float cosH = std::cos(a_anchorHeading);
		const float sinH = std::sin(a_anchorHeading);
		return {
			a_anchorPos.x + (a_placement.x * cosH - a_placement.y * sinH),
			a_anchorPos.y + (a_placement.x * sinH + a_placement.y * cosH),
			a_anchorPos.z + a_placement.z
		};
	}

	// Caller-built description of a staged scene (files not yet loaded);
	// GraphManager::PlaySceneStaged loads everything up front.
	struct ScenePlan
	{
		struct Stage
		{
			std::vector<std::string> files;                // one per actor
			std::vector<ParticipantPlacement> placements;  // empty = all zero
			float timer = 0.0f;                            // seconds; <= 0 = no auto-advance
			int32_t loops = 0;                              // completed clip loops; <= 0 = no auto-advance
			float blendIn = -1.0f;                          // per-stage blend-in secs; < 0 = use the plan blendIn
		};
		std::vector<Stage> stages;
		std::string animId;  // registry animation id ("" = ad-hoc scene)
		float speed = 1.0f;     // scene clock speed multiplier
		float blendIn = 0.4f;   // default per-participant blend-in secs (stages may override)
		bool loopWhole = false; // restart at stage 0 after the last stage instead of ending
		// When false, the scene does NOT teleport participants at start and the
		// compose-root pin is skipped — the rendered skeleton rides each actor's
		// live world transform (the "follow" mode for in-place / synced-in-
		// formation playback; see docs/ANCHORING.md). True = anchored paired-scene
		// behavior (teleport to anchor+offset, then pin).
		bool anchored = true;
	};

	class Scene
	{
	public:
		struct ParticipantSlot
		{
			std::shared_ptr<const OzzSkeleton> skeleton;
			std::shared_ptr<const OzzAnimation> anim;
			std::string file;  // source path as given (for GetCurrentAnimation)
		};

		struct StageData
		{
			float timer = 0.0f;     // <= 0: hold until SetStage/StopScene
			int32_t loops = 0;      // <= 0: no loop-count auto-advance
			float duration = 0.0f;  // clip length of this stage (seconds)
			std::vector<ParticipantSlot> participants;
			// Per-stage placements, seeded from the plan.
			std::vector<ParticipantPlacement> placements;
			float blendIn = 0.4f;  // blend-in secs applied when this stage activates
		};

		// snapshot returned by Advance: the time and stage this sample uses
		struct Tick
		{
			float time;
			uint32_t stage;
		};

		std::mutex lock;
		float duration = 0.0f;  // current stage's clip length; participants share phase
		bool looped = true;
		// Clock speed multiplier. Atomic: SetSpeed writes it from the Papyrus
		// thread (under a participant's graph lock) while Advance reads it under
		// the scene lock — different locks, so the field carries its own ordering.
		std::atomic<float> speed{ 1.0f };

		// World anchor set at scene start; all participant positions are expressed
		// relative to this. Const after PlayScene completes.
		RE::NiPoint3 anchorPos{};
		float anchorHeading = 0.0f;  // radians

		// When false, the scene does NOT teleport participants at start and the
		// compose-root pin is skipped — the rendered skeleton rides each actor's
		// live world transform (see docs/ANCHORING.md). Const after the scene is
		// published.
		bool anchored = true;

		// Restart at stage 0 after the last stage instead of ending (PlaySequence
		// whole-loop). Const after publish.
		bool loopWhole = false;

		// Registry animation id when the scene came from a pack definition;
		// "" for ad-hoc scenes. Const after publish.
		std::string animId;

		std::vector<std::shared_ptr<Graph>> participants;

		// CURRENT stage's placements. The compose-root pin reads this without the
		// scene lock, so stage switches overwrite elements in place and never
		// resize/reallocate (sized once at scene start).
		std::vector<ParticipantPlacement> placements;

		// Immutable after PlaySceneStaged publishes the scene — graphs read it
		// lock-free to fetch their slot when the stage changes.
		std::vector<StageData> stages;

		// Set by Advance when the last stage's timer expires; the update hook
		// consumes endQueued once and defers StopScene to the game thread.
		std::atomic<bool> ended{ false };
		std::atomic<bool> endQueued{ false };

		// Set (only) by StopSceneLocked / StopAll. Lifecycle latch read by the
		// deferred game-thread teardown tasks under `lock`.
		std::atomic<bool> stopped{ false };

		// Advances the shared clock once per frame (frame detected from the
		// update-token stream), auto-advancing the stage on timer expiry, and
		// returns the scene time + stage this sample should use. Called by each
		// participant graph from the update hook.
		Tick Advance(const void* a_token, float a_deltaTime);

		// Manual stage jump (also used for the initial stage before publish).
		// Resets the stage clock; false if out of range.
		bool SetStage(int32_t a_stage);

		// Participant count; placements is sized once at scene start, so this
		// is safe without the lock.
		size_t ParticipantCount() const { return placements.size(); }

	private:
		// caller holds `lock`
		void ApplyStageLocked(uint32_t a_stage);

		FrameClock clock;
		uint32_t currentStage = 0;
		float stageElapsed = 0.0f;  // time in the current stage (clock.time wraps, this doesn't)
		int32_t stageLoops = 0;     // completed wraps in the current stage; also the 0-based loop index
	};
}
