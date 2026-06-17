#pragma once

// Content-neutral synced scene: one shared clock drives every participant graph
// (frame-locked regardless of job-thread order). The scene owns a world anchor +
// per-participant offsets; the BGSModelNode::Update pin holds the rendered
// skeleton there each frame. Stages preload all clips at start; Advance()
// auto-advances on timer expiry or loop-count, or holds when both are <= 0; after
// the last stage it flags `ended` and the hook defers StopScene.
//
// Scene POLICY (undress, voice, camera/control, fades, callbacks) is OSF Intimacy.

#include "Animation/FrameClock.h"
#include "Animation/OzzTypes.h"

namespace OSF::Animation
{
	class Graph;

	// Offset from the scene anchor, rotated into the anchor's heading frame.
	struct ParticipantPlacement
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
		float heading = 0.0f;  // radians, relative to anchor heading
	};

	// Anchor + (x,y,z) rotated into the anchor heading. Single source for the
	// initial teleport and the compose-root pin. (heading applied separately.)
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

	// Why an auto-advancing scene reached its terminal stage. Reported to the Layer-B
	// auto-advance handler (GraphManager::SceneAutoEndHandler) so SceneRuntime can pick the
	// matching auto-edge: kTimer -> a `timer` edge; kLoops -> a `loops`/`end` edge.
	enum class SceneEndReason : std::uint8_t
	{
		kTimer,
		kLoops
	};

	// Caller's scene description (files not yet loaded); PlaySceneStaged loads them.
	struct ScenePlan
	{
		struct Stage
		{
			std::vector<std::string> files;                // one per actor
			std::vector<ParticipantPlacement> placements;  // empty = all zero
			float timer = 0.0f;                            // seconds; <= 0 = no auto-advance
			int32_t loops = 0;                             // clip loops; <= 0 = no auto-advance
			float blendIn = -1.0f;                         // secs; < 0 = use plan blendIn
		};
		std::vector<Stage> stages;
		std::string animId;     // registry id ("" = ad-hoc)
		float speed = 1.0f;     // clock speed multiplier
		float blendIn = 0.4f;   // default per-participant blend-in secs
		bool loopWhole = false; // restart at stage 0 after the last (vs end)
		// false = no teleport/pin; the rig rides each actor's live transform
		// ("follow", see docs/ANCHORING.md). true = teleport to anchor+offset, pin.
		bool anchored = true;
	};

	class Scene
	{
	public:
		struct ParticipantSlot
		{
			std::shared_ptr<const OzzSkeleton> skeleton;
			std::shared_ptr<const OzzAnimation> anim;
			std::string file;  // source path (for GetCurrentAnimation)
		};

		struct StageData
		{
			float timer = 0.0f;     // <= 0: hold
			int32_t loops = 0;      // <= 0: no loop-count advance
			float duration = 0.0f;  // clip length (s)
			std::vector<ParticipantSlot> participants;
			std::vector<ParticipantPlacement> placements;
			float blendIn = 0.4f;   // blend-in secs when this stage activates
		};

		struct Tick  // what this sample should use
		{
			float time;
			uint32_t stage;
		};

		std::mutex lock;
		float duration = 0.0f;  // current stage's clip length
		bool looped = true;
		// SetSpeed writes from the Papyrus thread (under a graph lock), Advance
		// reads under the scene lock — atomic carries its own ordering.
		std::atomic<float> speed{ 1.0f };

		// World anchor; const after start. Participant positions are relative to it.
		RE::NiPoint3 anchorPos{};
		float anchorHeading = 0.0f;  // radians

		bool anchored = true;    // see ScenePlan::anchored; const after publish
		bool loopWhole = false;  // const after publish
		std::string animId;      // registry id, "" for ad-hoc; const after publish

		std::vector<std::shared_ptr<Graph>> participants;

		// CURRENT stage's placements. The pin reads this lock-free, so stage
		// switches overwrite in place — sized once at start, never reallocated.
		std::vector<ParticipantPlacement> placements;

		// Immutable after publish — graphs read it lock-free on a stage change.
		std::vector<StageData> stages;

		// Advance sets `ended` after the last stage; the hook consumes `endQueued`
		// once and defers StopScene to the game thread.
		std::atomic<bool> ended{ false };
		std::atomic<bool> endQueued{ false };

		// Why the terminal stage ended (only meaningful once `ended` is set). Set under
		// `lock` in Advance; read by the deferred auto-end task to pick the auto-edge.
		std::atomic<SceneEndReason> endReason{ SceneEndReason::kLoops };

		// Advances the shared clock once per frame (owner-token gated), auto-advancing
		// stages, and returns the time + stage this sample should use.
		Tick Advance(const void* a_token, float a_deltaTime);

		// Manual stage jump (also the initial stage). Resets the stage clock;
		// false if out of range.
		bool SetStage(int32_t a_stage);

		// Authoritative current stage index — updated immediately by SetStage / auto-advance,
		// unlike the per-graph `appliedStage` which lags until the next sample. Caller must
		// NOT hold `lock`.
		uint32_t CurrentStage();

	private:
		void ApplyStageLocked(uint32_t a_stage);  // caller holds `lock`

		FrameClock clock;
		uint32_t currentStage = 0;
		float stageElapsed = 0.0f;  // time in stage (doesn't wrap, unlike clock.time)
		int32_t stageLoops = 0;     // completed wraps in stage = 0-based loop index
	};
}
