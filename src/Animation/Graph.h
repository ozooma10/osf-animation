#pragma once

// Per-actor graph: samples the active ozz clip and writes the pose into the engine's
// flat rig local buffers, which is the path the renderer reads from. The ozz plumbing
// is adapted from NativeAnimationFrameworkSF (GPL-3.0, Copyright (C) Deweh); the
// rig-buffer apply path follows what we reverse-engineered on 1.16.244.

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
		kAdditive = 1,  // root motion travels from the anchor (experimental — currently pins)
		kFollow = 2     // no pin — ride the actor's live transform
	};

	class Graph
	{
	public:
		std::mutex lock;
		RE::NiPointer<RE::TESObjectREFR> target;

		// guarded by `lock`
		std::shared_ptr<const OzzSkeleton> skeleton;
		std::shared_ptr<const OzzAnimation> anim;
		float localTime = 0.0f;
		float speed = 1.0f;
		bool looped = true;

		std::string currentFile;  // current clip path as given (for GetCurrentAnimation); "" = none

		// Per-graph world anchor (OSF.SetAnchor) for SOLO graphs; scene participants
		// are pinned by their scene. When hasAnchor && rootMode != kFollow the stamp
		// hook pins the compose root to anchorPos.
		bool hasAnchor = false;
		RE::NiPoint3 anchorPos{};
		RootMode rootMode = RootMode::kPin;

		// Non-null while in a Sync group: samples the group's shared clock instead of
		// soloClock. Cleared when the graph joins a Scene (the scene owns the clock).
		std::shared_ptr<SyncGroup> syncGroup;

		// Non-null while a scene participant; the scene owns the clock.
		Scene* scene = nullptr;
		int participantIndex = -1;   // index into scene->placements/participants; const while in the scene
		uint32_t appliedStage = 0;   // stage this graph's clip matches; Sample swaps on a stage change
		uint32_t sceneFrames = 0;    // update-call counter, diag cadence only

		// Cross-fade (guarded by `lock`). Blend-in at every SetAnimation: from the
		// previous sampled pose if any (clip-to-clip), else the engine's live rig
		// pose. Blend-out at BeginFadeOut: keep sampling while weight ramps to 0
		// (lands on the live pose), then the hook queues RemoveFadedGraph.
		BlendPhase blendPhase = BlendPhase::kNone;
		float blendDuration = 0.4f;  // seconds for either ramp
		bool removalQueued = false;  // fade-out finished, removal dispatched

		void BeginFadeOut();      // start the fade-out ramp (no-op if already fading)
		bool IsFadedOut() const;  // fade-out ramp fully elapsed

		// Scene teardown: detach, hand the clip to the solo clock at its current
		// phase (fade keeps animating), start the fade-out. Caller holds `lock`.
		void DetachAndFadeOut()
		{
			scene = nullptr;
			participantIndex = -1;
			soloClock.Reset();
			soloClock.time = localTime;
			BeginFadeOut();
		}

		// Last resolved 3D root (diag only). NiPointer not raw: the late
		// TESLoadGameEvent backstop can fire after 3D is torn down on other threads,
		// so a raw pointer would be a use-after-free. Cleared on a failed resolve.
		RE::NiPointer<RE::BSFadeNode> lastRoot;

		// modelNode identity the stamp hook matches against (set by Sample's bind).
		const RE::BGSModelNode* StampTarget() const { return cachedModelNode; }

		void SetAnimation(std::shared_ptr<const OzzSkeleton> a_skeleton, std::shared_ptr<const OzzAnimation> a_anim, std::string a_file = "");

		// AnimationManager::Update hook (job threads, ~7x/frame, subdivided dt).
		// Re-resolves the rig chain (caching the stamp hook's match key) and advances
		// time (a_token = clock owner), driving stage advance. Does NOT sample/write
		// the pose — StampPose does that once per frame (a write here would be
		// clobbered by the engine's snapshot applier).
		void Sample(float a_deltaTime, const void* a_token);

		// BGSModelNode::Update (vfunc 2) hook PRE-orig, on the thread that owns this
		// skeleton's compose: samples the clip at Sample's accumulated time and stamps
		// it into the rig local buffer, which the same call then composes and commits
		// (the write point we confirmed). No-op until a_modelNode matches the cached binding.
		void StampPose(const RE::BGSModelNode* a_modelNode);

	private:
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

		FrameClock soloClock;   // used only when `scene` is null
		FrameClock blendClock;  // blend ramps; owner-token gated, reset at SetAnimation/BeginFadeOut

		std::vector<ozz::math::Float4x4> blendFromPose;  // cross-fade-from pose (our joint indexing)
		bool blendFromValid = false;

		// one-shot diagnostics
		bool loggedBind = false;
		bool loggedFirstApply = false;
		bool loggedSampleFail = false;
	};
}
