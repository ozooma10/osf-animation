#pragma once

// Samples the active ozz clip into the engine's flat rig local buffers (the path the renderer reads).
// - The ozz plumbing is adapted from NativeAnimationFrameworkSF (GPL-3.0, Copyright (C) Deweh); 

#include "Animation/BoneMask.h"
#include "Animation/BlendSource.h"
#include "Animation/FrameClock.h"
#include "Animation/LiveBasePose.h"
#include "Animation/OzzTypes.h"
#include "Animation/Scene.h"

#include <array>

#include "ozz/animation/runtime/sampling_job.h"

namespace OSF::Animation
{
	enum class BlendPhase : std::uint8_t
	{
		kNone,
		kIn,
		kOut
	};

	enum class RootMode : std::uint8_t
	{
		kPin = 0,       // lock the rendered root at the anchor (root translation ignored)
		kFollow = 1
	};

	class Graph
	{
		friend class GraphManager;

	private:
		std::mutex stateLock;
		RE::NiPointer<RE::TESObjectREFR> target;

		// graph state is guarded by `stateLock`
		std::shared_ptr<const OzzSkeleton> skeleton;
		std::shared_ptr<const OzzAnimation> anim;
		float localTime = 0.0f;

		// Current clip file; "" = none.
		std::string currentFile;

		// Solo-graph anchor (OSF.SetAnchor); scene participants are pinned by their scene.
		bool hasAnchor = false;
		RE::NiPoint3 anchorPos{};
		RootMode rootMode = RootMode::kPin;
		std::uint64_t anchorRevision = 0;  // invalidates deferred SetAnchor teleports

		// Non-null for all non-scene graphs; the (possibly shared) group owns the clock.
		std::shared_ptr<SyncGroup> syncGroup;

		// Non-null while a scene participant; the scene owns the clock.
		PlaybackSession* playbackSession = nullptr;
		int participantIndex = -1;
		// Stage placement copied under stateLock so the compose hook never reads the scene's (data race).
		ParticipantPlacement scenePlacement{};
		ParticipantPlacement alignDelta{};
		uint32_t appliedStage = 0;

		// Blend-out keeps sampling while weight ramps to 0 (lands on the live pose), then queues RemoveFadedGraph.
		BlendPhase blendPhase = BlendPhase::kNone;
		float blendDuration = 0.4f;
		bool removalQueued = false;

		// Wall-clock stamps for the stranded-graph sweep: blendClock stalls when the engine stops updating the actor.
		std::atomic<std::int64_t> lastSampleMs{ 0 };  // also stamped at SetAnimation so a never-sampled graph ages from birth
		std::int64_t fadeOutBeganMs = 0;              // guarded by stateLock
		std::uint64_t playbackRevision = 0;           // invalidates deferred cleanup from an older clip

		// Call before replacement teardown clears the binding.
		void PrepareBlendSource();
		// a_file is diagnostics only.
		void SetAnimation(std::shared_ptr<const OzzSkeleton> a_skeleton, std::shared_ptr<const OzzAnimation> a_anim, std::string a_file = "");
		void SetPosePolicy(PoseMode a_mode, float a_weight, std::string a_roleName = "");
		// Case-insensitive rig bone names left engine-driven; replaced at every solo/scene start.
		void SetPreserveBones(const std::vector<std::string>& a_bones);
		// "" = none. With a mask only its bones are stamped (per-bone weighted); a change forces a rebind.
		void SetBoneMask(const std::string& a_maskName);

		void BeginFadeOut();
		bool IsFadedOut() const;

		// Engine stopped updating the actor (judged on wall clock, generous slack). Takes stateLock.
		bool IsStranded(std::int64_t a_nowMs) const;

		void DetachAndFadeOut();

		// Atomic: the compose hook uses it as a lock-free gate, then rechecks under stateLock.
		const RE::BGSModelNode* StampTarget() const
		{
			return cachedModelNode.load(std::memory_order_relaxed);
		}

		// AnimationManager::Update hook (job threads, ~7x/frame, subdivided dt): resolves the binding only;
		// a pose write here would be clobbered by the engine's snapshot applier.
		void Sample(float a_deltaTime, const void* a_token);

		// BGSModelNode::Update (vfunc 2) pre-hook: samples and stamps into the rig local buffer the same
		// call then composes. No-op until a_modelNode matches the cached binding.
		void StampPose(const RE::BGSModelNode* a_modelNode);

		// Non-null a_expectedModelNode: bind only if it is the actor's current node (compose-time recovery).
		bool ResolveAndBind(const RE::BGSModelNode* a_expectedModelNode = nullptr);
		// a_preserveLiveBase keeps the last proven engine base (by skeleton joint) across the rebind.
		void InvalidateBinding(bool a_preserveLiveBase = false);

		ozz::animation::SamplingJob::Context samplingContext;
		std::vector<ozz::math::SoaTransform> localPose;
		std::vector<ozz::math::Float4x4> outputPose;
		std::vector<ozz::math::Float4x4> referencePose;  // local rest pose
		std::unordered_map<std::string, uint16_t> jointMap;
		std::unordered_set<std::string> preserveBones;
		const BoneMask::Mask* boneMask = nullptr;            // static storage; null = every body bone binds
		PoseMode poseMode = PoseMode::kOverride;
		float poseWeight = 1.0f;
		std::string roleName;

		bool hasPose = false;  // also the blend-from source for the next SetAnimation

		std::atomic<const RE::BGSModelNode*> cachedModelNode{ nullptr };
		const RE::BGSModelNode::Rig* cachedRig = nullptr;
		uint32_t cachedBoneCount = 0;
		const void* cachedLocalData = nullptr;  // rig->local->data at bind time; a fresh buffer invalidates the cache
		uint16_t cachedRigBoneCount = 0;         // modelNode+0x78 at bind time; a rebuilt rig re-binds
		struct BoundBone
		{
			uint16_t rigIndex;
			uint16_t jointIndex;
			float weight;
		};
		std::vector<BoundBone> binding;
		std::vector<std::uint16_t> rigIndexByJoint;
		// Additive stamps over an immutable engine base, snapshotted once per enginePoseRevision so
		// repeated composes never layer OSF over itself.
		std::vector<float> liveBasePose;  // binding order, 16 floats per entry
		bool liveBaseValid = false;
		LiveBasePose::Cache liveBaseCache;
		std::uint64_t enginePoseRevision = 1;
		std::uint64_t basePoseRevision = 0;
		std::uint64_t bindingRevision = 0;
		LiveBasePose::Evaluation evaluatedBinding;
		// First slot bytes of the last stamp. enginePoseRevision counts update calls, not engine writes:
		// if the slot still holds our write, the engine hasn't re-evaluated and the old base is kept.
		std::array<float, 16> stampProbe{};
		uint16_t stampProbeRigIdx = 0;
		bool stampProbeValid = false;
		// glitch-frame diagnostics (guarded by stateLock)
		std::uint32_t captureGateCount = 0;
		std::uint32_t rebindBaseCarryCount = 0;
		std::uint32_t staleStampRebindCount = 0;
		std::uint32_t composeResolveFailCount = 0;

		// One-shot final stage: clamp at the last frame instead of wrapping. Survives DetachAndFadeOut
		// so the fade-out doesn't snap to frame 0.
		bool holdClipAtEnd = false;

		FrameClock blendClock;  // owner-token gated

		std::vector<ozz::math::Float4x4> blendFromPose;
		std::vector<std::uint8_t> blendFromDriven;       // joints stamped before the stage/clip change
		BlendSource::Prepared<ozz::math::Float4x4> preparedBlendSource;
		bool blendFromValid = false;

		bool loggedBind = false;
		bool loggedFirstApply = false;
		bool loggedSampleFail = false;
		bool loggedAdditivePlayback = false;
		bool loggedMaskBind = false;
	};
}
