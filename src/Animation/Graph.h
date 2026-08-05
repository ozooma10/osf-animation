#pragma once

// Animation Graph run on an actor.
// - Sample active ozz clip and writes the pose into engines flat rig local buffers, which is the path the renderer reads from.
// - The ozz plumbing is adapted from NativeAnimationFrameworkSF (GPL-3.0, Copyright (C) Deweh); 

#include "Animation/BoneMask.h"
#include "Animation/BlendSource.h"
#include "Animation/FrameClock.h"
#include "Animation/LiveBasePose.h"
#include "Animation/OzzTypes.h"
#include "Animation/Scene.h"  // ParticipantPlacement

#include <array>

#include "ozz/animation/runtime/sampling_job.h"

namespace OSF::Animation
{
	enum class BlendPhase : std::uint8_t
	{
		kNone,  // stamp at full weight
		kIn,    // ramping 0 -> 1 after SetAnimation
		kOut    // ramping 1 -> 0 after BeginFadeOut; graph removed at 0
	};

	// How a graph's root-bone translation meets its anchor.
	enum class RootMode : std::uint8_t
	{
		kPin = 0,       // lock the rendered root at the anchor (root translation ignored)
		kFollow = 1     // no pin — ride the actor's live transform
	};

	class Graph
	{
		// GraphManager owns the graph's lifetime and drives Sample/StampPose from the per-frame hooks 
		// it is the only thing that touches a Graph's internals.
		friend class GraphManager;

	private:
		std::mutex stateLock;
		RE::NiPointer<RE::TESObjectREFR> target;

		// graph state is guarded by `stateLock`
		std::shared_ptr<const OzzSkeleton> skeleton;
		std::shared_ptr<const OzzAnimation> anim;
		float localTime = 0.0f;

		// Currently running animclip file; "" = none.
		std::string currentFile;

		// Per-graph world anchor (OSF.SetAnchor) for SOLO graphs; scene participants are pinned by their scene. 
		// When hasAnchor && rootMode != kFollow the stamp hook pins the compose root to anchorPos.
		bool hasAnchor = false;
		RE::NiPoint3 anchorPos{};
		RootMode rootMode = RootMode::kPin;
		std::uint64_t anchorRevision = 0;  // invalidates deferred SetAnchor teleports

		//non-null for all non-scene graphs. sync merges members onto a shared group (which owns the clock)
		std::shared_ptr<SyncGroup> syncGroup;

		// Non-null while a scene participant; the scene owns the clock.
		Scene* scene = nullptr;
		int participantIndex = -1;   // index into scene participants; const while in the scene
		// Current stage placement copied while applying the stage under stateLock. The compose hook
		// reads this graph-owned value under the same lock, avoiding a scene placement data race.
		ParticipantPlacement scenePlacement{};
		uint32_t appliedStage = 0;   // stage this graph's clip matches; Sample swaps on a stage change
		uint32_t sceneFrames = 0;    // update-call counter for the HoldAnchoredParticipant re-assert cadence

		// Handle cross fade timing. blend-in from previous pose (sampled or engines live rig)
		// blend-out at BeginFadeOut: keep sampling while weight ramps to 0 (lands on the live pose), then the hook queues RemoveFadedGraph.	
		BlendPhase blendPhase = BlendPhase::kNone;
		float blendDuration = 0.4f;  // seconds for either ramp
		bool removalQueued = false;  // fade-out finished, removal dispatched

		// Wall-clock stamps for the stranded-graph sweep. blendClock only advances inside Sample(),
		// which requires the engine to keep animation-updating the actor — when it stops (actor
		// unloaded / AI-disabled), a fade can never elapse and a solo loop can never end, so the
		// sweep judges by steady-clock instead.
		std::atomic<std::int64_t> lastSampleMs{ 0 };  // last Sample() call (stamped at SetAnimation too, so a never-sampled graph ages from birth)
		std::int64_t fadeOutBeganMs = 0;              // stamped at BeginFadeOut; guarded by stateLock
		std::uint64_t playbackRevision = 0;           // incremented by SetAnimation; invalidates deferred cleanup from an older clip


		// Capture the currently displayed graph pose before replacement teardown clears its binding.
		void PrepareBlendSource();
		// Start a new animation clip. Resets time and starts a blend-in. a_file is for diagnostics only ("" = none).
		void SetAnimation(std::shared_ptr<const OzzSkeleton> a_skeleton, std::shared_ptr<const OzzAnimation> a_anim, std::string a_file = "");
		// Per-role local-pose composition. This is independent of root anchoring and survives stage changes.
		void SetPosePolicy(PoseMode a_mode, float a_weight, std::string a_roleName = "");
		// Exact, case-insensitive rig bone names whose live engine transforms this graph must not overwrite.
		// The policy survives stage changes; GraphManager replaces it at every new solo/scene start.
		void SetPreserveBones(const std::vector<std::string>& a_bones);
		// Named driven-bone whitelist ("" = none): with a mask this graph stamps ONLY the mask's
		// bones (per-bone weighted), leaving the rest of the rig engine-driven — the partial-body
		// gesture path. Forces a rebind when it changes; a scene stage may override the role default.
		void SetBoneMask(const std::string& a_maskName);
		void SetContactPose(const ContactPose& a_contactPose);

		void BeginFadeOut();      // start the fade-out ramp (no-op if already fading)
		bool IsFadedOut() const;  // fade-out ramp fully elapsed

		// The engine stopped updating this actor: a fade can never elapse (blendClock only advances
		// in Sample) and a solo loop can never end. Judged on wall clock with generous slack; the
		// caller (stranded-graph sweep / RemoveFadedGraph) is pause/resume filtered. Holds stateLock.
		bool IsStranded(std::int64_t a_nowMs) const;

		// Scene teardown: detach, hand clip back to "solo" syncGroup, and begin the exit blend.
		void DetachAndFadeOut();

		// modelNode identity the stamp hook matches against (set by Sample's bind).
		// Published atomically because the compose hook uses it as a lock-free fast-path gate,
		// then verifies it again while holding stateLock before stamping.
		const RE::BGSModelNode* StampTarget() const
		{
			return cachedModelNode.load(std::memory_order_relaxed);
		}

		// AnimationManager::Update hook (job threads, ~7x/frame, subdivided dt). Re-resolves the rig chain (caching the stamp hooks match key)
		// Does NOT sample/write the pose, StampPose does that once per frame (a write here would be clobbered by the engine's snapshot applier).
		void Sample(float a_deltaTime, const void* a_token);

		// BGSModelNode::Update (vfunc 2) hook PRE-orig, on the thread that owns this skeleton's compose
		// samples the clip at Sample's accumulated time and stamps it into the rig local buffer, which the same call then composes and commits
		// No-op until a_modelNode matches the cached binding.
		void StampPose(const RE::BGSModelNode* a_modelNode);

		// When a_expectedModelNode is non-null, bind only if it is exactly the actor's
		// current node. The compose hook uses this to recover a temporarily lost binding
		// on the new node's first update without accepting an unrelated skeleton.
		bool ResolveAndBind(const RE::BGSModelNode* a_expectedModelNode = nullptr);
		// Drop the cached modelNode/rig identity and binding so the next Sample re-resolves.
		// Runtime-only rebinds retain the last proven engine base by skeleton joint.
		void InvalidateBinding(bool a_preserveLiveBase = false);

		ozz::animation::SamplingJob::Context samplingContext;
		std::vector<ozz::math::SoaTransform> localPose;
		std::vector<ozz::math::Float4x4> outputPose;
		std::vector<ozz::math::Float4x4> referencePose;  // active skeleton's local rest pose, parallel to outputPose
		std::unordered_map<std::string, uint16_t> jointMap;  // lowercased joint name -> index
		std::unordered_set<std::string> preserveBones;       // lowercased live rig names omitted from binding
		const BoneMask::Mask* boneMask = nullptr;            // static-storage named mask; null = every body bone binds
		PoseMode poseMode = PoseMode::kOverride;
		float poseWeight = 1.0f;
		std::string roleName;  // diagnostics only

		bool hasPose = false;  // outputPose valid (also the blend-from source for the next SetAnimation)

		// resolved fresh each frame; binding cached against modelNode/rig identity
		std::atomic<const RE::BGSModelNode*> cachedModelNode{ nullptr };
		const RE::BGSModelNode::Rig* cachedRig = nullptr;
		uint32_t cachedBoneCount = 0;
		const void* cachedLocalData = nullptr;  // rig->local->data at bind time; a reused modelNode with a fresh buffer invalidates the cache
		uint16_t cachedRigBoneCount = 0;         // rigBoneCount (modelNode+0x78) at bind time; part of the cache identity so a rebuilt rig re-binds
		struct BoundBone
		{
			uint16_t rigIndex;
			uint16_t jointIndex;
			float weight;  // active mask's per-bone weight; 1 when unmasked
			float contactWeight;  // contact-only contribution; zero for ordinary mask bones
		};
		std::vector<BoundBone> binding;
		std::vector<std::uint16_t> rigIndexByJoint;
		ContactPose contactPose;
		std::unordered_set<std::string> contactPoseBones;
		bool loggedContactPoseFull = false;
		// Additive stamping reads an immutable engine base. Sample increments enginePoseRevision only
		// after the engine graph evaluation; the first compose for that revision snapshots each bound
		// live slot. Any repeated compose call reuses that snapshot, so OSF never layers over itself.
		std::vector<float> liveBasePose;  // binding order, 16 floats per entry; allocated only on bind
		bool liveBaseValid = false;
		LiveBasePose::Cache liveBaseCache;  // stable joint-indexed carry-over across rig-slot remaps
		std::uint64_t enginePoseRevision = 1;
		std::uint64_t basePoseRevision = 0;
		std::uint64_t bindingRevision = 0;
		LiveBasePose::Evaluation evaluatedBinding;
		// The bytes of the first slot the last StampPose wrote. enginePoseRevision counts
		// update-stream calls, not proven engine buffer writes: before adopting the buffer as a
		// fresh live base, the capture compares this probe — if the slot still holds OUR write,
		// the engine has not re-evaluated and the previous base is kept (never our own output).
		std::array<float, 16> stampProbe{};
		uint16_t stampProbeRigIdx = 0;
		bool stampProbeValid = false;
		// glitch-frame diagnostics (all guarded by stateLock)
		std::uint32_t captureGateCount = 0;        // live-base captures rejected by the probe
		std::uint32_t rebindBaseCarryCount = 0;    // compose rebinds that reused the prior proven base
		std::uint32_t staleStampRebindCount = 0;   // same-address node-table mutations healed at compose
		std::uint32_t composeResolveFailCount = 0; // compose-time recovery attempts that failed

		// The scene finished a one-shot final stage: clamp the clip clock at its final frame
		// instead of wrapping. Survives DetachAndFadeOut so the fade-out also holds the end pose
		// (a wrap during the stop/fade window snapped the actor to the clip's first frame).
		bool holdClipAtEnd = false;

		FrameClock blendClock;  // blend ramps; owner-token gated, reset at SetAnimation/BeginFadeOut

		std::vector<ozz::math::Float4x4> blendFromPose;  // cross-fade-from pose (our joint indexing)
		std::vector<std::uint8_t> blendFromDriven;       // joints actually stamped before the stage/clip change
		BlendSource::Prepared<ozz::math::Float4x4> preparedBlendSource;  // survives replacement teardown invalidating binding
		bool blendFromValid = false;

		// one-shot diagnostics
		bool loggedBind = false;
		bool loggedFirstApply = false;
		bool loggedSampleFail = false;
		bool loggedAdditivePlayback = false;
		bool loggedMaskBind = false;
	};
}
