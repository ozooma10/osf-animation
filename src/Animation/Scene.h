#pragma once

// A synced scene: one shared clock drives every participant graph, so they stay
// frame-locked no matter what order the job threads run in. The scene owns a world
// anchor and per-participant offsets; the BGSModelNode::Update pin holds the rendered
// skeleton there each frame. All of a stage's clips are preloaded at start; Advance()
// auto-advances on a timer or a loop count, or just holds when both are <= 0; after
// the last stage it flags `ended` and the hook defers the StopScene.
//
// This class is pure playback. The policy that sits on top of it (equipment, voice,
// camera/control, fades, callbacks) lives in the scene runtime, not here.

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

	// Why an auto-advancing scene reached its final stage. Reported back to the scene
	// runtime's auto-advance handler so it can pick the matching auto-edge: kTimer -> a
	// `timer` edge; kLoops -> a `loops`/`end` edge.
	enum class SceneEndReason : std::uint8_t
	{
		kTimer,
		kLoops
	};

	// A timed mark on a stage's clip timeline — basically "fire this opaque token at time
	// T, on lane L". The scene runtime builds these from a node's tracks (cue/action/sound/
	// camera) and decodes the lane+token on the way back. The scene fires purely by time and
	// never interprets either field. (Enter/exit lifecycle entries are fired by the runtime
	// directly, not through this list.)
	struct TimedMark
	{
		float         fraction = 0.0f;   // clip-local fraction in [0,1); ignored when atEnd
		bool          everyLoop = false;  // fire every loop (else first loop only)
		bool          atEnd = false;      // fire once at the clip end of the first loop
		std::uint8_t  lane = 0;          // opaque lane id (the runtime assigns meaning + ordering)
		std::string   token;             // opaque payload (cue id, action index, ...)
	};

	// A mark the scene fired this frame, drained by the hook and handed to the runtime. The
	// lane+token are exactly what the runtime stamped onto the TimedMark; the scene just
	// round-trips them.
	struct FiredMark
	{
		std::uint8_t lane = 0;
		std::string  token;
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
			std::vector<TimedMark> marks;                  // timed marks (numeric/end) for this stage
		};
		std::vector<Stage> stages;
		std::string animId;     // registry id ("" = ad-hoc)
		float speed = 1.0f;     // clock speed multiplier
		float blendIn = 0.4f;   // default per-participant blend-in secs
		bool loopWhole = false; // restart at stage 0 after the last (vs end)
		// false = no teleport or pin; the rig follows each actor's live transform.
		// true = teleport to anchor+offset and pin there.
		bool anchored = true;
		// Explicit world anchor (StartSceneAt). false = anchor at actor[0]'s current
		// transform (the default). true = anchor at anchorPos/anchorHeading instead, so a
		// scene can be world-anchored to a piece of furniture/marker, not an actor.
		bool anchorExplicit = false;
		RE::NiPoint3 anchorPos{};
		float anchorHeading = 0.0f;  // radians
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
			std::vector<TimedMark> marks;  // timed marks fired by Advance (see firedMarks)
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

		// Move the marks fired since the last drain into a_out (swaps the buffer empty).
		// Drained once per frame by the update hook; the runtime decodes lane+token. The
		// caller must NOT hold `lock`.
		void DrainFiredMarks(std::vector<FiredMark>& a_out);

	private:
		void ApplyStageLocked(uint32_t a_stage);  // caller holds `lock`

		FrameClock clock;
		uint32_t currentStage = 0;
		float stageElapsed = 0.0f;  // time in stage (doesn't wrap, unlike clock.time)
		int32_t stageLoops = 0;     // completed wraps in stage = 0-based loop index

		// Timed-mark scheduling state (all under `lock`). firedMarks accumulates the marks
		// (lane+token) whose times crossed this frame; markFired[i] gates a non-repeating mark
		// to fire once per stage pass (parallel to the current stage's `marks`, reset on every
		// stage change).
		std::vector<FiredMark> firedMarks;
		std::vector<bool>      markFired;
	};
}
