#pragma once

// Animation Graph run on an actor.
// - Sample active ozz clip and writes the pose into engines flat rig local buffers, which is the path the renderer reads from.
// - The ozz plumbing is adapted from NativeAnimationFrameworkSF (GPL-3.0, Copyright (C) Deweh); 

#include "Animation/FrameClock.h"
#include "Animation/OzzTypes.h"

#include "ozz/animation/runtime/sampling_job.h"

namespace OSF::Animation
{
	class Scene;

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

		//non-null for all non-scene graphs. sync merges members onto a shared group (which owns the clock)
		std::shared_ptr<SyncGroup> syncGroup;

		// Non-null while a scene participant; the scene owns the clock.
		Scene* scene = nullptr;
		int participantIndex = -1;   // index into scene->placements/participants; const while in the scene
		uint32_t appliedStage = 0;   // stage this graph's clip matches; Sample swaps on a stage change
		uint32_t sceneFrames = 0;    // update-call counter for the HoldAnchoredParticipant re-assert cadence

		// Handle cross fade timing. blend-in from previous pose (sampled or engines live rig)
		// blend-out at BeginFadeOut: keep sampling while weight ramps to 0 (lands on the live pose), then the hook queues RemoveFadedGraph.	
		BlendPhase blendPhase = BlendPhase::kNone;
		float blendDuration = 0.4f;  // seconds for either ramp
		bool removalQueued = false;  // fade-out finished, removal dispatched


		// Start a new animation clip. Resets time and starts a blend-in. a_file is for diagnostics only ("" = none).
		void SetAnimation(std::shared_ptr<const OzzSkeleton> a_skeleton, std::shared_ptr<const OzzAnimation> a_anim, std::string a_file = "");

		void BeginFadeOut();      // start the fade-out ramp (no-op if already fading)
		bool IsFadedOut() const;  // fade-out ramp fully elapsed

		// Scene teardown: detach, hand clip back to "solo" syncGroup
		void DetachAndFadeOut()
		{
			scene = nullptr;
			participantIndex = -1;
			syncGroup = std::make_shared<SyncGroup>();	// return to a "solo" syncGroup (group of 1)
			syncGroup->clock.time = localTime;			// fade resumes from current phase
			BeginFadeOut();
		}

		// modelNode identity the stamp hook matches against (set by Sample's bind).
		const RE::BGSModelNode* StampTarget() const { return cachedModelNode; }

		// AnimationManager::Update hook (job threads, ~7x/frame, subdivided dt). Re-resolves the rig chain (caching the stamp hooks match key)
		// Does NOT sample/write the pose, StampPose does that once per frame (a write here would be clobbered by the engine's snapshot applier).
		void Sample(float a_deltaTime, const void* a_token);

		// BGSModelNode::Update (vfunc 2) hook PRE-orig, on the thread that owns this skeleton's compose
		// samples the clip at Sample's accumulated time and stamps it into the rig local buffer, which the same call then composes and commits
		// No-op until a_modelNode matches the cached binding.
		void StampPose(const RE::BGSModelNode* a_modelNode);

		bool ResolveAndBind();

		ozz::animation::SamplingJob::Context samplingContext;
		std::vector<ozz::math::SoaTransform> localPose;
		std::vector<ozz::math::Float4x4> outputPose;
		std::unordered_map<std::string, uint16_t> jointMap;  // lowercased joint name -> index

		bool hasPose = false;  // outputPose valid (also the blend-from source for the next SetAnimation)

		// resolved fresh each frame; binding cached against modelNode/rig identity
		const RE::BGSModelNode* cachedModelNode = nullptr;
		const RE::BGSModelNode::Rig* cachedRig = nullptr;
		uint32_t cachedBoneCount = 0;
		std::vector<std::pair<uint16_t, uint16_t>> binding;  // {rigIndex, jointIndex}

		FrameClock blendClock;  // blend ramps; owner-token gated, reset at SetAnimation/BeginFadeOut

		std::vector<ozz::math::Float4x4> blendFromPose;  // cross-fade-from pose (our joint indexing)
		bool blendFromValid = false;

		// one-shot diagnostics
		bool loggedBind = false;
		bool loggedFirstApply = false;
		bool loggedSampleFail = false;
	};
}
