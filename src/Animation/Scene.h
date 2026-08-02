#pragma once

// A synced scene: one shared clock drives every participant graph, so they stay frame-locked no matter what order the job threads run in. 
// The scene owns a world anchor and per-participant offsets; the BGSModelNode::Update pin holds the rendered skeleton there each frame. 
// All of a stage's clips are preloaded at start; Advance() auto-advances on a timer or a loop count, or just holds when both are <= 0; 
// after the last stage it flags `ended` and the hook defers the StopScene.

#include "Animation/BoneMask.h"
#include "Animation/FrameClock.h"
#include "Animation/OzzTypes.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace OSF::Animation
{
	class Graph;
	using PlaybackId = std::uint64_t;

	// How an authored scene role contributes its sampled local pose. This is deliberately
	// separate from RootMode / ScenePlan::anchored: pose composition is not root anchoring.
	enum class PoseMode : std::uint8_t
	{
		kOverride,
		kAdditive
	};

	// Authored pose weights clamp into the public [0,1] contract. Non-finite values are
	// rejected by the registry (nullopt) instead of silently poisoning stamp math.
	inline std::optional<float> NormalizePoseWeight(double a_weight)
	{
		if (!std::isfinite(a_weight)) {
			return std::nullopt;
		}
		return static_cast<float>(std::clamp(a_weight, 0.0, 1.0));
	}

	// Offset from the scene anchor, rotated into the anchor's heading frame.
	struct ParticipantPlacement
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
		float heading = 0.0f;  // radians, relative to anchor heading
	};

	// Anchor + (x,y,z) rotated into the anchor heading. Single source for the initial teleport and the compose-root pin. (heading applied separately.)
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

	// Why an auto-advancing scene reached its final stage.
	// Reported back to the scene runtime's auto-advance handler so it can pick the matching auto-edge: kTimer -> a `timer` edge; kLoops -> a `loops`/`end` edge.
	// kInterrupted is NOT a natural completion: the stall watchdog sets it when the engine stopped ticking
	// the scene (actor unloaded / AI-disabled / interrupted), and the handler force-ends (takes no edge).
	enum class SceneEndReason : std::uint8_t
	{
		kTimer,
		kLoops,
		kInterrupted
	};

	// A timed mark on a stage's clip timeline - basically "fire this opaque token at time  T, on lane L". 
	// The scene runtime builds these from a node's tracks (cue/action/sound/camera) and decodes the lane+token on the way back. 
	// The scene fires purely by time and never interprets either field. (Enter/exit lifecycle entries are fired by the runtime directly, not through this list.)
	struct TimedMark
	{
		float         fraction = 0.0f;   // clip-local fraction in [0,1); ignored when atEnd or seconds >= 0
		float         seconds = -1.0f;   // clip-local SECONDS (an authored frame); < 0 = position by `fraction`
		bool          everyLoop = false;  // fire every loop (else first loop only)
		bool          atEnd = false;      // fire once at the clip end of the first loop
		std::uint8_t  lane = 0;          // opaque lane id (the runtime assigns meaning + ordering)
		std::string   token;             // opaque payload (cue id, action index, ...)
	};

	// Where a mark sits on the clip's own clock. An absolute mark (`seconds`, authored as a frame)
	// stays put however long the clip turns out to be; a fractional one scales with it. A mark past
	// the clip end never lands inside a loop's [0, duration) window, so it simply never fires.
	inline float MarkTime(const TimedMark& a_mark, float a_duration)
	{
		return a_mark.seconds >= 0.0f ? a_mark.seconds : a_mark.fraction * a_duration;
	}

	// A mark the scene fired this frame, drained by the hook and handed to the runtime. 
	// The lane+token are exactly what the runtime stamped onto the TimedMark; the scene just round-trips them.
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
			std::vector<std::string> animIds;              // optional, parallel to files; empty string = first/default animation
			std::vector<ParticipantPlacement> placements;  // empty = all zero
			std::vector<std::string> masks;                 // optional, one effective bone mask per actor for this stage
			float timer = 0.0f;                            // seconds; <= 0 = no auto-advance
			int32_t loops = 0;                             // clip loops; <= 0 = no auto-advance
			float hold = -1.0f;                            // freeze on ONE frame at this clip position [0,1]; < 0 = play normally
			float blendIn = -1.0f;                         // secs; < 0 = use plan blendIn
			std::vector<TimedMark> marks;                  // timed marks (numeric/end) for this stage
		};
		std::vector<Stage> stages;
		std::vector<std::vector<std::string>> preserveBones;  // optional, one exact-name list per actor
		std::vector<PoseMode> poseModes;                       // optional, one mode per actor; empty = all override
		std::vector<float> poseWeights;                        // optional, one normalized [0,1] weight per actor; empty = all 1
		std::vector<std::string> masks;                        // optional, one named bone mask per actor ("" = unmasked); empty = all unmasked
		std::vector<std::string> roleNames;                    // optional diagnostics, one role name per actor
		std::string animId;     // registry id ("" = ad-hoc)
		float speed = 1.0f;     // clock speed multiplier
		float blendIn = 0.4f;   // default per-participant blend-in secs
		bool loopWhole = false; // restart at stage 0 after the last (vs end)

		// false = no teleport or pin; the rig follows each actor's live transform.
		// true = teleport to anchor+offset and pin there.
		bool anchored = true;
		
		// Explicit world anchor (StartSceneAt). false = anchor at actor[0]'s current transform (the default). 
		// true = anchor at anchorPos/anchorHeading instead, so a scene can be world-anchored to a piece of furniture/marker, not an actor.
		bool anchorExplicit = false;

		RE::NiPoint3 anchorPos{};
		float anchorHeading = 0.0f;  // radians

		// Pre-scene participant transforms to restore on teardown, parallel to the actors (empty =
		// sample at start, the default). A caller replacing its own scene for the same cast (browser
		// stage switch) supplies the original baseline here: the actors are already placed by the
		// outgoing scene when the replacement starts, so sampling then would "restore" them to the
		// old scene's placement instead of where they stood before any scene ran.
		std::vector<std::pair<RE::NiPoint3, float>> baselineTransforms;
	};

	// ScenePlan is internal, but several construction paths build it. Keep every optional per-role
	// policy vector all-or-none so actor/role indexing can never silently drift.
	inline bool HasValidRolePolicyShape(const ScenePlan& a_plan, std::size_t a_actorCount)
	{
		const auto optionalCountMatches = [a_actorCount](std::size_t a_size) {
			return a_size == 0 || a_size == a_actorCount;
		};
		if (!optionalCountMatches(a_plan.preserveBones.size()) ||
			!optionalCountMatches(a_plan.poseModes.size()) ||
			!optionalCountMatches(a_plan.poseWeights.size()) ||
			!optionalCountMatches(a_plan.masks.size()) ||
			!optionalCountMatches(a_plan.roleNames.size())) {
			return false;
		}
		return std::ranges::all_of(a_plan.poseModes, [](PoseMode a_mode) {
			return a_mode == PoseMode::kOverride || a_mode == PoseMode::kAdditive;
		}) && std::ranges::all_of(a_plan.poseWeights, [](float a_weight) {
			return std::isfinite(a_weight) && a_weight >= 0.0f && a_weight <= 1.0f;
		}) && std::ranges::all_of(a_plan.masks, [](const std::string& a_mask) {
			return a_mask.empty() || BoneMask::Find(a_mask) != nullptr;
		}) && std::ranges::all_of(a_plan.stages, [a_actorCount](const ScenePlan::Stage& a_stage) {
			return (a_stage.masks.empty() || a_stage.masks.size() == a_actorCount) &&
				std::ranges::all_of(a_stage.masks, [](const std::string& a_mask) {
					return a_mask.empty() || BoneMask::Find(a_mask) != nullptr;
				});
		});
	}

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
			// Freeze-frame stage: the clock parks at hold * duration and the clip never advances or
			// wraps, so only `timer` (or a manual advance/stop) leaves it. < 0 = a normal playing stage.
			float hold = -1.0f;
			float duration = 0.0f;  // clip length (s)
			std::vector<ParticipantSlot> participants;
			std::vector<ParticipantPlacement> placements;
			std::vector<std::string> masks;  // effective per-participant mask for this stage
			float blendIn = 0.4f;   // blend-in secs when this stage activates
			std::vector<TimedMark> marks;  // timed marks fired by Advance (see firedMarks)
		};

		struct Tick  // what this sample should use
		{
			float time;
			uint32_t stage;
			// The scene finished a one-shot final stage (ended by loop count): participants hold
			// the clip's FINAL frame — never wrap back to frame 0 — while the deferred stop and
			// the fade-out land. Without this the cast snapped to the clip's start pose for
			// however many frames the stop task took to arrive.
			bool holdEnd = false;
		};
		struct DiagnosticSnapshot
		{
			float time;
			uint32_t stage;
			const void* owner;
			std::int64_t ownerAgeMs;
		};
		struct PlaybackSnapshot
		{
			float time;
			float duration;
			float speed;
			uint32_t stage;
		};

		std::mutex lock;
		float duration = 0.0f;  // current stage's clip length
		// SetSpeed writes from the Papyrus thread (under a graph lock), Advance
		// reads under the scene lock — atomic carries its own ordering.
		std::atomic<float> speed{ 1.0f };

		// World anchor; const after start. Participant positions are relative to it.
		RE::NiPoint3 anchorPos{};
		float anchorHeading = 0.0f;  // radians

		bool anchored = true;    // see ScenePlan::anchored; const after publish
		bool restoreParticipantTransforms = false;  // explicit world/furniture anchors return actors to their pre-scene transforms on normal teardown
		bool loopWhole = false;  // const after publish
		std::string animId;      // registry id, "" for ad-hoc; const after publish

		std::vector<std::shared_ptr<Graph>> participants;
		std::vector<std::pair<RE::NiPoint3, float>> originalTransforms;  // participant position + heading, parallel to participants

		// Immutable after publish — graphs read it lock-free on a stage change.
		std::vector<StageData> stages;

		// Advance sets `ended` after the last stage; the hook consumes `endQueued` once and defers StopScene to the game thread.
		std::atomic<bool> ended{ false };
		std::atomic<bool> endQueued{ false };

		// Steady-clock (ms) of the last frame ANY participant ticked this scene (written at the top of
		// Advance; seeded at build time by BuildSceneFromPlan so a never-ticked scene still stall-detects).
		// The stall watchdog reads it: a live scene whose timestamp goes stale while the game is running
		// has been interrupted (the engine stops AnimationManager::Update for unloaded / AI-disabled
		// actors), so it ends cleanly instead of stranding participants + locks.
		std::atomic<std::int64_t> lastAdvanceMs{ 0 };
		// Compose-hook heartbeat and Trace-log throttle. These distinguish a stopped update clock from
		// a clock that advances while the rendered pose stops stamping.
		std::atomic<std::int64_t> lastStampMs{ 0 };
		std::atomic<std::int64_t> lastDiagnosticMs{ 0 };

		// Why the terminal stage ended (only meaningful once `ended` is set). 
		// Set under `lock` in Advance; read by the deferred auto-end task to pick the auto-edge.
		std::atomic<SceneEndReason> endReason{ SceneEndReason::kLoops };

		// Stable identity for this concrete playback instance. Runtime callbacks validate it so a
		// deferred task from an old node can never act on a replacement using the same actors.
		PlaybackId playbackId = 0;
		std::uint64_t worldEpoch = 0;

		// Advances the shared clock once per frame (owner-token gated), auto-advancing stages, and returns the time + stage this sample should use.
		Tick Advance(const void* a_token, float a_deltaTime);

		// Manual stage jump (also the initial stage). Resets the stage clock; false if out of range.
		bool SetStage(int32_t a_stage);

		// Move within the current stage without firing timed marks. Marks before the new time are
		// treated as consumed so resuming playback cannot replay authored side effects.
		bool Seek(float a_time);

		PlaybackSnapshot GetPlaybackSnapshot();

		// Authoritative current stage index - updated immediately by SetStage / auto-advance, unlike the per-graph `appliedStage` which lags until the next sample.
		// Caller must NOT hold `lock`.
		uint32_t CurrentStage();

		// Snapshot clock-owner state under the scene lock for rate-limited Trace diagnostics.
		DiagnosticSnapshot GetDiagnosticSnapshot(std::int64_t a_nowMs);

		// Move the marks fired since the last drain into a_out (swaps the buffer empty).
		// Drained once per frame by the update hook; the runtime decodes lane+token. 
		// The caller must NOT hold `lock`.
		void DrainFiredMarks(std::vector<FiredMark>& a_out);

	private:
		void ApplyStageLocked(uint32_t a_stage);  // caller holds `lock`

		FrameClock clock;
		uint32_t currentStage = 0;
		float stageElapsed = 0.0f;  // time in stage (doesn't wrap, unlike clock.time)
		int32_t stageLoops = 0;     // completed wraps in stage = 0-based loop index
		// A frozen stage (StageData::hold) fires the marks at or before its hold point ONCE, on the
		// first update after it activates; this gates that pass (reset by every stage change).
		bool holdMarksFired = false;

		// Timed-mark scheduling state (all under `lock`). firedMarks accumulates the marks  (lane+token) whose times crossed this frame; 
		// markFired[i] gates a non-repeating mark to fire once per stage pass (parallel to the current stage's `marks`, reset on every stage change).
		std::vector<FiredMark> firedMarks;
		std::vector<bool>      markFired;
	};
}
