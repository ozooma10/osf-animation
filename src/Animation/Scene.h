#pragma once

// Synchronized playback: one shared clock drives every participant graph so they stay frame-locked
// regardless of job-thread order. All segment clips are preloaded at start.

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
	using PlaybackSinkId = std::uint64_t;

	enum class PoseMode : std::uint8_t
	{
		kOverride,
		kAdditive
	};

	// Clamps to [0,1]; non-finite -> nullopt (rejected by the registry).
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

	// Blends a few clip bones in local-pose space around an authored contact time, then eases back
	// to the engine pose.
	struct ContactPose
	{
		bool enabled = false;
		float atSeconds = 0.0f;
		std::vector<std::string> bones;
		float approachSeconds = 0.4f;
		float fullBeforeSeconds = 2.0f / 30.0f;
		float fullAfterSeconds = 2.0f / 30.0f;
		float releaseSeconds = 0.4f;

		float WeightAt(float a_time) const
		{
			if (!enabled) return 0.0f;
			const float begin = atSeconds - approachSeconds;
			const float fullBegin = atSeconds - fullBeforeSeconds;
			const float fullEnd = atSeconds + fullAfterSeconds;
			const float end = fullEnd + releaseSeconds;
			const auto smooth = [](float a_t) {
				const float t = std::clamp(a_t, 0.0f, 1.0f);
				return t * t * (3.0f - 2.0f * t);
			};
			if (a_time <= begin || a_time >= end) return 0.0f;
			if (a_time < fullBegin) return smooth((a_time - begin) / std::max(fullBegin - begin, 1e-5f));
			if (a_time <= fullEnd) return 1.0f;
			return 1.0f - smooth((a_time - fullEnd) / std::max(end - fullEnd, 1e-5f));
		}
	};

	// Single source for the initial teleport and the compose-root pin; heading applied separately.
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

	// kInterrupted comes from the stall watchdog (engine stopped ticking); the handler force-ends
	// without taking an edge.
	enum class PlaybackEndReason : std::uint8_t
	{
		kTimerComplete,
		kLoopComplete,
		kInterrupted,

		kTimer = kTimerComplete,
		kLoops = kLoopComplete
	};

	// Opaque lane+token fired at a time on a stage's clip timeline; the runtime assigns meaning.
	struct TimedMark
	{
		float         fraction = 0.0f;   // clip-local fraction in [0,1); ignored when atEnd or seconds >= 0
		float         seconds = -1.0f;   // clip-local seconds; < 0 = position by `fraction`
		bool          everyLoop = false;  // fire every loop (else first loop only)
		bool          atEnd = false;      // fire once at the clip end of the first loop
		std::uint8_t  lane = 0;
		std::string   token;
	};

	// Absolute marks (`seconds`) ignore clip length; marks past the clip end never fire.
	inline float MarkTime(const TimedMark& a_mark, float a_duration)
	{
		return a_mark.seconds >= 0.0f ? a_mark.seconds : a_mark.fraction * a_duration;
	}

	struct FiredMark
	{
		std::uint8_t lane = 0;
		std::string  token;
	};

	// `resourcePath` may use NAF/GLB clip-spec syntax; resolved only at the GraphManager boundary.
	struct AnimationClipSpec
	{
		std::string resourcePath;
		std::string animationId;
	};

	enum class WorldPlacementMode : std::uint8_t
	{
		kAnchorAndPin,
		kFollowActor
	};

	inline constexpr WorldPlacementMode WorldPlacementFromFollowActor(bool a_followActor) noexcept
	{
		return a_followActor ? WorldPlacementMode::kFollowActor : WorldPlacementMode::kAnchorAndPin;
	}

	inline constexpr WorldPlacementMode WorldPlacementFromLegacyInPlace(bool a_inPlace) noexcept
	{
		return WorldPlacementFromFollowActor(a_inPlace);
	}

	struct PlaybackPlan
	{
		struct Segment
		{
			std::vector<std::string> files;
			std::vector<std::string> animIds;              // optional, parallel to files; "" = first/default animation
			std::vector<ParticipantPlacement> placements;  // empty = all zero
			std::vector<std::string> masks;
			std::vector<PoseMode> poseModes;               // optional stage override; empty = plan policy
			std::vector<float> poseWeights;                // optional stage override; empty = plan policy
			std::vector<ContactPose> contactPose;
			float timer = 0.0f;                            // seconds; <= 0 = no auto-advance
			int32_t loops = 0;                             // clip loops; <= 0 = no auto-advance
			float hold = -1.0f;                            // freeze at this clip position [0,1]; < 0 = play normally
			float blendIn = -1.0f;                         // secs; < 0 = use plan blendIn
			std::vector<TimedMark> marks;
		};
		using Stage = Segment;
		std::vector<Segment> stages;
		std::vector<std::vector<std::string>> preserveBones;
		std::vector<PoseMode> poseModes;                       // optional, one mode per actor; empty = all override
		std::vector<float> poseWeights;                        // [0,1]; empty = all 1
		std::vector<std::string> masks;                        // optional, one per actor ("" = unmasked); empty = all unmasked
		std::vector<std::string> roleNames;                    // diagnostics only
		std::string animId;     // registry id ("" = ad-hoc)
		float speed = 1.0f;
		float blendIn = 0.4f;   // secs
		bool loopWhole = false; // restart at stage 0 after the last (vs end)

		// false = follow each actor's live transform (no teleport/pin); true = teleport to anchor+offset and pin.
		bool anchored = true;

		[[nodiscard]] WorldPlacementMode GetWorldPlacementMode() const noexcept
		{
			return anchored ? WorldPlacementMode::kAnchorAndPin : WorldPlacementMode::kFollowActor;
		}

		void SetWorldPlacementMode(WorldPlacementMode a_mode) noexcept
		{
			anchored = a_mode == WorldPlacementMode::kAnchorAndPin;
		}
		
		// false = anchor at actor[0]'s transform; true = anchorPos/anchorHeading (furniture/marker).
		bool anchorExplicit = false;

		RE::NiPoint3 anchorPos{};
		float anchorHeading = 0.0f;  // radians

		// Pre-scene transforms restored on teardown (empty = sample at start). A same-cast replacement passes
		// the original baseline, since its actors already stand where the outgoing scene put them.
		std::vector<std::pair<RE::NiPoint3, float>> baselineTransforms;
	};

	// Optional per-role policy vectors are all-or-none so actor/role indexing can't drift.
	inline bool HasValidRolePolicyShape(const PlaybackPlan& a_plan, std::size_t a_actorCount)
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
		}) && std::ranges::all_of(a_plan.stages, [a_actorCount](const PlaybackPlan::Segment& a_stage) {
			return (a_stage.masks.empty() || a_stage.masks.size() == a_actorCount) &&
				(a_stage.contactPose.empty() || a_stage.contactPose.size() == a_actorCount) &&
				(a_stage.poseModes.empty() || a_stage.poseModes.size() == a_actorCount) &&
				(a_stage.poseWeights.empty() || a_stage.poseWeights.size() == a_actorCount) &&
				std::ranges::all_of(a_stage.poseModes, [](PoseMode a_mode) {
					return a_mode == PoseMode::kOverride || a_mode == PoseMode::kAdditive;
				}) && std::ranges::all_of(a_stage.poseWeights, [](float a_weight) {
					return std::isfinite(a_weight) && a_weight >= 0.0f && a_weight <= 1.0f;
				}) &&
				std::ranges::all_of(a_stage.masks, [](const std::string& a_mask) {
					return a_mask.empty() || BoneMask::Find(a_mask) != nullptr;
				});
		});
	}

	class PlaybackSession
	{
	public:
		struct ParticipantSlot
		{
			std::shared_ptr<const OzzSkeleton> skeleton;
			std::shared_ptr<const OzzAnimation> anim;
			std::string file;
		};

		struct SegmentData
		{
			float timer = 0.0f;     // <= 0: hold
			int32_t loops = 0;      // <= 0: no loop-count advance
			// Freeze frame: clock parks at hold * duration; only `timer` or a manual advance/stop leaves.
			// < 0 = a normal playing stage.
			float hold = -1.0f;
			float duration = 0.0f;  // clip length (s)
			std::vector<ParticipantSlot> participants;
			std::vector<ParticipantPlacement> placements;
			std::vector<std::string> masks;
			std::vector<PoseMode> poseModes;
			std::vector<float> poseWeights;
			std::vector<ContactPose> contactPose;
			float blendIn = 0.4f;
			std::vector<TimedMark> marks;
		};
		using StageData = SegmentData;

		struct Tick
		{
			float time;
			uint32_t stage;
			// One-shot final stage ended: hold the clip's last frame (no wrap to 0) while the deferred stop
			// and fade-out land.
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
		float duration = 0.0f;
		// Written from Papyrus (under a graph lock), read under the scene lock.
		std::atomic<float> speed{ 1.0f };

		// World anchor; const after start. Participant positions are relative to it.
		RE::NiPoint3 anchorPos{};
		float anchorHeading = 0.0f;  // radians

		bool anchored = true;    // const after publish
		bool restoreParticipantTransforms = false;
		bool loopWhole = false;  // const after publish
		std::string animId;      // "" = ad-hoc; const after publish

		std::vector<std::shared_ptr<Graph>> participants;
		std::vector<std::pair<RE::NiPoint3, float>> originalTransforms;  // position + heading

		// Immutable after publish — graphs read it lock-free on a stage change.
		std::vector<StageData> stages;

		// Advance sets `ended` after the last stage; the hook consumes `endQueued` once and defers StopScene to the game thread.
		std::atomic<bool> ended{ false };
		std::atomic<bool> endQueued{ false };

		// Last frame any participant ticked (steady ms; seeded at build). Goes stale when the engine stops
		// updating unloaded/AI-disabled actors, so the stall watchdog ends the scene.
		std::atomic<std::int64_t> lastAdvanceMs{ 0 };
		// Trace-only compose heartbeat: tells a stopped clock from one that ticks without stamping.
		std::atomic<std::int64_t> lastStampMs{ 0 };
		std::int64_t lastDiagnosticMs{ 0 };  // main-thread maintenance only

		// Valid once `ended`; set under `lock` in Advance, read by the auto-end task to pick the edge.
		std::atomic<PlaybackEndReason> endReason{ PlaybackEndReason::kLoopComplete };

		// Callbacks validate it so a deferred task can't act on a replacement with the same actors.
		PlaybackId playbackId = 0;
		PlaybackSinkId playbackSinkId = 0;  // 0 = standalone playback (no callback owner)
		std::uint64_t worldEpoch = 0;

		Tick Advance(const void* a_token, float a_deltaTime);

		// Also sets the initial stage. Resets the stage clock; false if out of range.
		bool SetSegment(int32_t a_segment);

		// Doesn't fire marks; marks before the new time count as consumed so side effects never replay.
		bool Seek(float a_time);

		PlaybackSnapshot GetPlaybackSnapshot();

		// Updated immediately, unlike per-graph `appliedStage`. Caller must NOT hold `lock`.
		uint32_t CurrentSegment();

		DiagnosticSnapshot GetDiagnosticSnapshot(std::int64_t a_nowMs);

		// Swaps out marks fired since the last drain. Caller must NOT hold `lock`.
		void DrainFiredMarks(std::vector<FiredMark>& a_out);

	private:
		void ApplySegmentLocked(uint32_t a_segment);  // caller holds `lock`

		FrameClock clock;
		uint32_t currentSegment = 0;
		float stageElapsed = 0.0f;  // time in stage (doesn't wrap, unlike clock.time)
		int32_t stageLoops = 0;     // completed wraps in stage = 0-based loop index
		// A frozen stage fires marks at/before its hold point once, on its first update.
		bool holdMarksFired = false;

		// Under `lock`. markFired gates non-repeating marks once per stage pass (parallel to the stage's marks).
		std::vector<FiredMark> firedMarks;
		std::vector<bool>      markFired;
	};
}
