#pragma once

// Per-actor animation graph: samples the active ozz clip and writes the pose
// into the engine's flat rig local buffers (the pipeline the renderer actually
// consumes on current patches — see the CommonLibSF BGSModelNode/BSFadeNode
// layouts and the 1.16.242 RE ground truth in DESIGN.md).
// Ozz plumbing ported from NativeAnimationFrameworkSF (GPL-3.0, Copyright (C)
// Deweh); the rig-buffer apply path follows 1.16.242 RE ground truth.

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

	// How a graph's own root-bone translation meets its anchor. See
	// docs/ANCHORING.md. (additive is experimental — currently pins like kPin.)
	enum class RootMode : std::uint8_t
	{
		kPin = 0,       // lock the rendered root at the anchor (root translation ignored)
		kAdditive = 1,  // root motion travels from the anchor (experimental)
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

		// Source file of the currently-loaded clip, as the caller passed it
		// (relative to Data). Retained for OSF.GetCurrentAnimation; "" = none.
		std::string currentFile;

		// Optional per-graph world anchor (OSF.SetAnchor) for SOLO graphs; scene
		// participants are pinned by their scene instead. When hasAnchor and
		// rootMode != kFollow, the stamp hook pins the compose root to anchorPos.
		bool hasAnchor = false;
		RE::NiPoint3 anchorPos{};
		RootMode rootMode = RootMode::kPin;

		// Non-null while this SOLO graph is part of a Sync group: it then samples
		// at the group's shared clock instead of its own soloClock. Cleared when
		// the graph joins a Scene (the scene owns the clock).
		std::shared_ptr<SyncGroup> syncGroup;

		// non-null while this graph is part of a synced scene; the scene then
		// owns the clock and this graph samples at the shared scene time.
		Scene* scene = nullptr;

		// index into scene->placements (and scene->participants); set at scene
		// start, const while the scene lives.
		int participantIndex = -1;

		// the scene stage this graph's animation currently matches; when the
		// scene's stage moves on, Sample swaps in the preloaded slot for the new
		// stage (each graph swaps itself — no cross-graph locking in the hook)
		uint32_t appliedStage = 0;

		// update-hook call counter from scene start — used for diag cadence only
		uint32_t sceneFrames = 0;

		// Cross-fade state (guarded by `lock`). Blend-in starts at every
		// SetAnimation: from the previous sampled pose if one exists (stage
		// transitions / replays cross-fade clip-to-clip), else from the
		// engine's live rig pose read at the write point (scene/solo start).
		// Blend-out starts at BeginFadeOut: the graph keeps sampling while its
		// stamp weight ramps to 0 — landing on the engine's live pose — then
		// the update hook queues RemoveFadedGraph on the game thread.
		BlendPhase blendPhase = BlendPhase::kNone;
		float blendDuration = 0.4f;  // seconds for either ramp
		bool removalQueued = false;  // fade-out finished, removal dispatched

		// Starts the fade-out ramp (no-op if already fading out).
		void BeginFadeOut();

		// True once the fade-out ramp has fully elapsed.
		bool IsFadedOut() const;

		// Scene teardown: detach from the scene, hand the clip to the solo
		// clock at its current phase (so the fade keeps animating instead of
		// restarting), and start the fade-out. Caller holds `lock`.
		void DetachAndFadeOut()
		{
			scene = nullptr;
			participantIndex = -1;
			soloClock.Reset();
			soloClock.time = localTime;
			BeginFadeOut();
		}

		// last resolved 3D root (diagnostics only — the visual position is the
		// root node's world transform, which physics can push independently of
		// the refr location fields). NiPointer, not raw: the late
		// TESLoadGameEvent backstop still fires after the engine has torn 3D
		// down on other threads, so a raw pointer here is a use-after-free
		// window. Cleared whenever the resolve fails so the diag never reads a
		// detached root.
		RE::NiPointer<RE::BSFadeNode> lastRoot;

		// modelNode identity for the stamp hook's lookup (set by Sample's bind,
		// compared against the BGSModelNode::Update `this`)
		const RE::BGSModelNode* StampTarget() const { return cachedModelNode; }

		void SetAnimation(std::shared_ptr<const OzzSkeleton> a_skeleton, std::shared_ptr<const OzzAnimation> a_anim, std::string a_file = "");

		// Called from the AnimationManager::Update hook (job threads, ~7x per
		// render frame with subdivided dt). Re-resolves the rig chain from the
		// actor (caching modelNode identity + name binding — this establishes the
		// stamp hook's match key) and advances time (a_token = the
		// AnimationManager pointer = clock owner key), driving stage advance and
		// cue/voice firing off the slice stream. Does NOT sample or write the
		// pose: the ozz evaluation is deferred to StampPose so it runs once per
		// render frame instead of once per slice (only the composed pose is ever
		// rendered, and the engine's snapshot applier would clobber a write here).
		void Sample(float a_deltaTime, const void* a_token);

		// Called from the BGSModelNode::Update (vfunc 2) hook PRE-orig, on the
		// scene-update thread that owns this skeleton's compose: samples the
		// active clip at the time Sample accumulated, then stamps that pose into
		// the rig local buffer, which the same call composes and commits
		// (RE-proven write point). Runs once per render frame per skeleton.
		// No-op until a_modelNode matches the binding Sample cached.
		void StampPose(const RE::BGSModelNode* a_modelNode);

	private:
		bool ResolveAndBind();

		ozz::animation::SamplingJob::Context samplingContext;
		std::vector<ozz::math::SoaTransform> localPose;
		std::vector<ozz::math::Float4x4> outputPose;
		std::unordered_map<std::string, uint16_t> jointMap;  // lowercased joint name -> joint index

		// true once StampPose has produced a valid outputPose (also the
		// blend-from source captured by the next SetAnimation)
		bool hasPose = false;

		// resolved fresh each frame; binding cached against modelNode/rig identity
		const RE::BGSModelNode* cachedModelNode = nullptr;
		const RE::BGSModelNode::Rig* cachedRig = nullptr;
		uint32_t cachedBoneCount = 0;
		std::vector<std::pair<uint16_t, uint16_t>> binding;  // {rigIndex, jointIndex}

		FrameClock soloClock;  // used only when `scene` is null

		// Wall-time clock for the blend ramps (owner-token gated like the play
		// clocks, so subdivided update calls advance it exactly 1x). Reset at
		// SetAnimation and BeginFadeOut; independent of clip looping/clamping.
		FrameClock blendClock;

		// pose snapshot to cross-fade FROM (previous clip's last sampled pose,
		// our joint indexing); only valid when blendFromValid
		std::vector<ozz::math::Float4x4> blendFromPose;
		bool blendFromValid = false;

		// one-shot diagnostics
		bool loggedBind = false;
		bool loggedFirstApply = false;
		bool loggedSampleFail = false;
	};
}
