#include "Graph.h"

#include "Animation/PoseMath.h"
#include "Animation/Scene.h"
#include "Util/StringUtil.h"

#include "ozz/base/span.h"

#include <chrono>
#include <cmath>

namespace OSF::Animation
{
	namespace
	{
		using OSF::Util::ToLower;

		// OSF drives the BODY only — the face and eyes stay with the engine's own systems (look-at,
		// blink, facial anim). Face bones are prefixed "faceBone_" and morph targets "morph". The
		// eyeball look-at bones are NOT faceBone_-prefixed (L_Eye/R_Eye/Eye_Target), so they must be
		// named explicitly: the flat SAF/NAF clips author these with large (~0.7-1.0m) translations
		// in a look-at space, and because the engine parents them under C_Head, stamping those as
		// head-local offsets flings the eye meshes ~70 units off the head to a scene-dependent spot
		// (the "eyes popping out to a random location" report). ~45% of Gergel Ebanex clips animate
		// them. Skipping keeps the eyes engine-driven. (Sibling head bones — tongue/ears/DirectAt —
		// measured <0.15m across the pack, so they compose fine and are left drivable.)
		bool IsNonBodyRigNode(std::string_view a_lowerName)
		{
			return a_lowerName.starts_with("facebone") || a_lowerName.starts_with("morph") ||
			       a_lowerName == "l_eye" || a_lowerName == "r_eye" || a_lowerName == "eye_target";
		}

		// Write bone slot in engines NiTransform layout. Rotation as 3 rows of 4 floats (0x00,0x10,0x20), translation +0x30, scale +0x3C 
		// row-vector convention so rows are transpose of standard column-vec matrix (byte identical to ozz's column-major)
		// BGSModelNode+0x78 = u16 rigBoneCount: element count of rig->local/world/prevWorld.
		constexpr std::uintptr_t kModelNodeRigBoneCountOffset = 0x78;
		inline uint16_t GetRigBoneCount(const RE::BGSModelNode* a_modelNode)
		{
			return *reinterpret_cast<const uint16_t*>(
				reinterpret_cast<std::uintptr_t>(a_modelNode) + kModelNodeRigBoneCountOffset);
		}

		void WriteNiTransformRows(float* a_slot, const ozz::math::Float4x4& a_matrix)
		{
			const float* m = reinterpret_cast<const float*>(&a_matrix);
			std::memcpy(a_slot, m, 60);  // 3 basis columns (= engine rows) + translation
			a_slot[15] = 1.0f;           // uniform scale
		}

		void WriteNiTransformRowsBlended(float* a_slot, const float* a_from, const ozz::math::Float4x4& a_target, float a_weight)
		{
			PoseMath::WriteOverrideBlended(a_slot, a_from, reinterpret_cast<const float*>(&a_target), a_weight);
		}
	}

	void Graph::SetAnimation(std::shared_ptr<const OzzSkeleton> a_skeleton, std::shared_ptr<const OzzAnimation> a_anim, std::string a_file)
	{
		if (++playbackRevision == 0) {
			++playbackRevision;  // reserve zero for a graph that has never started
		}

		//crossfade from pose on screen when there is one. otherwise blend in from engines live pose
		if (hasPose && !outputPose.empty() &&
			outputPose.size() == static_cast<size_t>(a_skeleton->data->num_joints())) {
			blendFromPose = outputPose;
			blendFromValid = true;
		} else {
			blendFromValid = false;
		}
		blendPhase = BlendPhase::kIn;
		blendClock.Reset();
		removalQueued = false;
		lastSampleMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);

		skeleton = std::move(a_skeleton);
		anim = std::move(a_anim);
		currentFile = std::move(a_file);
		localTime = 0.0f;
		hasPose = false;
		if(!scene) {
			syncGroup = std::make_shared<SyncGroup>();	// new clip owns the clock if not in a scene
		}

		const int numJoints = skeleton->data->num_joints();
		samplingContext.Resize(numJoints);
		localPose.resize(skeleton->data->num_soa_joints());
		outputPose.assign(numJoints, ozz::math::Float4x4::identity());
		referencePose.assign(numJoints, ozz::math::Float4x4::identity());
		const auto restPose = skeleton->data->joint_rest_poses();
		UnpackSoaTransforms({ restPose.data(), restPose.size() },
			{ referencePose.data(), referencePose.size() }, skeleton->data.get());

		jointMap.clear();
		auto jointNames = skeleton->data->joint_names();
		for (size_t i = 0; i < jointNames.size(); i++) {
			jointMap[ToLower(jointNames[i])] = static_cast<uint16_t>(i);
		}

		// force a re-bind against the (possibly unchanged) rig
		cachedModelNode = nullptr;
		cachedRig = nullptr;
		cachedBoneCount = 0;
		cachedLocalData = nullptr;
		cachedRigBoneCount = 0;
		binding.clear();
		liveBasePose.clear();
		basePoseRevision = 0;
	}

	void Graph::SetPosePolicy(PoseMode a_mode, float a_weight, std::string a_roleName)
	{
		poseMode = a_mode;
		poseWeight = std::isfinite(a_weight) ? std::clamp(a_weight, 0.0f, 1.0f) : 1.0f;
		roleName = std::move(a_roleName);
		loggedAdditivePlayback = false;
		basePoseRevision = 0;
	}

	void Graph::SetPreserveBones(const std::vector<std::string>& a_bones)
	{
		preserveBones.clear();
		preserveBones.reserve(a_bones.size());
		for (const auto& bone : a_bones) {
			if (!bone.empty()) {
				preserveBones.emplace(ToLower(bone));
			}
		}

		// The binding is the write policy. Force it to rebuild even if the live rig itself did not change.
		cachedModelNode = nullptr;
		cachedRig = nullptr;
		cachedBoneCount = 0;
		cachedLocalData = nullptr;
		cachedRigBoneCount = 0;
		binding.clear();
		liveBasePose.clear();
		basePoseRevision = 0;
	}

	bool Graph::ResolveAndBind()
	{

		//failure invalidates the binding cache.
		//cachedModelNode is the stamp hooks match key. Once chain stops resolving (3d detatched), the cached address can be freed and reused.
		const auto fail = [this]() {
			cachedModelNode = nullptr;
			cachedRig = nullptr;
			cachedBoneCount = 0;
			cachedLocalData = nullptr;
			cachedRigBoneCount = 0;
			binding.clear();
			liveBasePose.clear();
			basePoseRevision = 0;
			return false;
		};

		auto* refr = target.get();
		if (!refr) {
			return fail();
		}

		// refr -> data3D -> BSFadeNode -> BGSModelNode -> rig -> local buffer.
		RE::NiPointer<RE::BSFadeNode> root;
		{
			auto loadedData = refr->loadedData.LockRead();
			if (*loadedData == nullptr) {
				return fail();
			}
			root.reset(static_cast<RE::BSFadeNode*>((*loadedData)->data3D.get()));
		}
		if (!root) {
			return fail();
		}

		auto* modelNode = root->bgsModelNode.get();
		if (!modelNode) {
			return fail();
		}
		auto* rig = modelNode->rig;
		if (!rig || !rig->local || !rig->local->data) {
			return fail();
		}

		// Cache identity includes the buffer base (rig->local->data) and rigBoneCount so a modelNode
		// that was freed and reused at the same address with a fresh/smaller rig buffer forces a rebind
		// instead of reusing stale rigIndices that now point past the live buffer.
		if (modelNode == cachedModelNode && rig == cachedRig
			&& rig->local->data == cachedLocalData
			&& modelNode->nodes.size() == cachedBoneCount
			&& GetRigBoneCount(modelNode) == cachedRigBoneCount) {
			return !binding.empty();
		}

		// DIAG: a rebind to a DIFFERENT non-null modelNode after the first bind means the actor's 3D was
		// rebuilt under us (e.g. equipment restore at scene end). Tells us whether a FADING graph re-binds to
		// the freshly-built skeleton and could keep stamping it. Once per swap.
		if (cachedModelNode && cachedModelNode != modelNode) {
			REX::TRACE("[Anim] rig REBIND — modelNode {} -> {} (3d rebuilt; blendPhase {})",
				static_cast<const void*>(cachedModelNode), static_cast<const void*>(modelNode),
				static_cast<int>(blendPhase));
		}

		// build the rigIndex -> jointIndex binding from the bone map
		cachedModelNode = modelNode;
		cachedRig = rig;
		cachedBoneCount = modelNode->nodes.size();
		cachedLocalData = rig->local->data;
		cachedRigBoneCount = GetRigBoneCount(modelNode);
		binding.clear();
		binding.reserve(cachedBoneCount);

		uint32_t skippedNonBody = 0;
		uint32_t skippedPreserved = 0;
		for (uint32_t i = 0; i < modelNode->nodes.size(); i++) {
			const auto& entry = modelNode->nodes[i];
			if (!entry.node) {
				continue;
			}
			const char* name = entry.node->name.c_str();
			if (!name) {
				continue;
			}
			const auto lowerName = ToLower(name);
			if (preserveBones.contains(lowerName)) {
				skippedPreserved++;
				continue;
			}
			if (IsNonBodyRigNode(lowerName)) {
				skippedNonBody++;
				continue;
			}
			if (auto iter = jointMap.find(lowerName); iter != jointMap.end()) {
				if (entry.rigIndex >= cachedRigBoneCount) {
					continue;  // node maps to a rig slot outside the live buffer; never stamp it
				}
				binding.emplace_back(entry.rigIndex, iter->second);
			}
		}
		// Allocate the immutable live-base buffer only when the binding changes, never in StampPose.
		liveBasePose.assign(binding.size() * 16, 0.0f);
		basePoseRevision = 0;

		if (!loggedBind) {
			loggedBind = true;
			REX::DEBUG("[Anim] rig bind — {}/{} mapped body bones matched skeleton joints ({} preserved, {} face/eye/morph nodes skipped)",
				binding.size(), cachedBoneCount, skippedPreserved, skippedNonBody);
		}
		if (poseMode == PoseMode::kAdditive && !loggedAdditivePlayback) {
			loggedAdditivePlayback = true;
			REX::DEBUG("[Anim] additive playback — actor {:08X}, role '{}', pose weight {:.3f}, {} driven bone(s), {} preserved bone(s)",
				target ? target->formID : 0, roleName.empty() ? "<anonymous>" : roleName,
				poseWeight, binding.size(), skippedPreserved);
		}

		return !binding.empty();
	}

	void Graph::BeginFadeOut()
	{
		if (blendPhase == BlendPhase::kOut) {
			return;
		}
		blendPhase = BlendPhase::kOut;
		blendClock.Reset();
		removalQueued = false;
		fadeOutBeganMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		// Drop the model-node cache: if the engine stops sampling this graph mid-fade (actor
		// unloaded), a stale cachedModelNode could later match a REUSED address and stamp a
		// foreign actor's rig. Sample re-resolves and re-binds on the next update, so a live
		// fade keeps stamping.
		cachedModelNode = nullptr;
		cachedRig = nullptr;
		cachedBoneCount = 0;
		cachedLocalData = nullptr;
		cachedRigBoneCount = 0;
		binding.clear();
		liveBasePose.clear();
		basePoseRevision = 0;
	}

	void Graph::DetachAndFadeOut()
	{
		scene = nullptr;
		participantIndex = -1;
		syncGroup = std::make_shared<SyncGroup>();  // return to a "solo" syncGroup (group of 1)
		syncGroup->clock.time = localTime;          // fade resumes from current phase

		// Face/eye/morph nodes are always left to Starfield, but C_Head is normally body-driven so
		// authored head motion still plays. At scene end the facial system and equipment rebuilds
		// resume immediately; continuing to blend C_Head for another 0.4s can make those dependent
		// nodes cache against a half-OSF/half-engine head pose. The player camera-mode rebuild clears
		// that stale state, but NPCs keep the malformed head. Hand C_Head back on the first teardown
		// frame while the rest of the body still fades smoothly. (BeginFadeOut below drops the cached
		// binding, so the rebind picks the new preserve set up.)
		preserveBones.emplace("c_head");

		BeginFadeOut();
	}
	bool Graph::IsFadedOut() const
	{
		return blendPhase == BlendPhase::kOut && blendClock.time >= blendDuration;
	}

	bool Graph::IsStranded(std::int64_t a_nowMs) const
	{
		constexpr std::int64_t kFadeSlackMs = 2000;  // beyond the ramp before a wall-clock fade counts as stranded
		constexpr std::int64_t kStarvedMs = 5000;    // unsampled this long (game running) = engine dropped the actor
		if (blendPhase == BlendPhase::kOut && fadeOutBeganMs > 0) {
			const auto rampMs = static_cast<std::int64_t>(blendDuration * 1000.0f);
			return a_nowMs - fadeOutBeganMs > rampMs + kFadeSlackMs;
		}
		const std::int64_t lastSample = lastSampleMs.load(std::memory_order_relaxed);
		return lastSample != 0 && a_nowMs - lastSample > kStarvedMs;
	}

	void Graph::Sample(float a_deltaTime, const void* a_token)
	{
		if (++enginePoseRevision == 0) {
			++enginePoseRevision;  // zero is reserved for an uncaptured base
		}
		lastSampleMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);

		// Blend ramps run on playback dt, independent of clip looping/clamping
		if (blendPhase != BlendPhase::kNone && blendClock.ShouldAdvance(a_token)) {
			blendClock.time += a_deltaTime;
		}

		// Advance scene clock first, if it triggers stage switch (new anim), clears rig binding, which will cause below to rebind in same call.
		if (scene) {
			const auto tick = scene->Advance(a_token, a_deltaTime);
			if (tick.stage != appliedStage && participantIndex >= 0 && tick.stage < scene->stages.size()) {
				const auto& stage = scene->stages[tick.stage];
				const auto& slot = stage.participants[participantIndex];
				SetAnimation(slot.skeleton, slot.anim, slot.file);
				blendDuration = stage.blendIn;  // per-stage blend-in
				scenePlacement = stage.placements[participantIndex];
				appliedStage = tick.stage;
			}
			localTime = tick.time;
			// The scene's stage boundary uses its reference clip, but malformed packs can contain
			// slightly different participant durations. Keep every SamplingJob ratio in [0,1) by
			// wrapping against this graph's own clip instead of feeding ozz an invalid ratio.
			if (anim && anim->data && anim->data->duration() > 0.0f) {
				localTime = std::fmod(localTime, anim->data->duration());
			}
		}

		if (!anim || !skeleton) {
			return;
		}

		if (!ResolveAndBind()) {
			return;
		}

		const float duration = anim->data->duration();
		if (duration <= 0.0f) {
			return;
		}

		//solo scenes ownly owner token advances time (so if ex. 1st/3rd person graph running, only 1 triggers time)
		if (!scene && syncGroup) {
			std::scoped_lock sgl{ syncGroup->lock };
			auto& clk = syncGroup->clock;
			const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
			if (clk.ShouldAdvance(a_token, nowMs)) {
				clk.time += a_deltaTime * syncGroup->speed.load(std::memory_order_relaxed);
			}
			float t = clk.time;
			t = std::fmod(t, duration);
			if (t < 0.0f) {
				t += duration;
			}
			localTime = t;
		}

		// ozz sample defered to StampPose. AnimationManager::Update fires 7x per frame, so sampling here is waste.
		// this only advance time/stage/cues and keeps rig binding warm. StampPose samples once per frame at time accumulated here
	}

	void Graph::StampPose(const RE::BGSModelNode* a_modelNode)
	{
		if (a_modelNode != cachedModelNode || binding.empty() || !anim || !skeleton ||
			outputPose.size() != referencePose.size()) {
			return;
		}

		//blend toward pose. slot content here is engines live pose for this frame, so blending blends against whatever actor would otherwise be doing.
		float weight = 1.0f;
		if (blendPhase == BlendPhase::kIn) {
			weight = blendDuration > 0.0f ? std::min(blendClock.time / blendDuration, 1.0f) : 1.0f;
			if (weight >= 1.0f) {
				blendPhase = BlendPhase::kNone;
			}
		} else if (blendPhase == BlendPhase::kOut) {
			weight = blendDuration > 0.0f ? 1.0f - std::min(blendClock.time / blendDuration, 1.0f) : 0.0f;
			if (weight <= 0.0f) {
				return;  // fully faded — leave the engine pose alone (no need to sample)
			}
		}

		// sample active clip at the time Sample accumulated on the update stream.
		const float duration = anim->data->duration();
		if (duration <= 0.0f) {
			return;
		}
		ozz::animation::SamplingJob job;
		job.animation = anim->data.get();
		job.context = &samplingContext;
		job.ratio = localTime / duration;
		job.output = ozz::make_span(localPose);
		if (!job.Run()) {
			if (!loggedSampleFail) {
				loggedSampleFail = true;
				REX::ERROR("[Anim] ozz SamplingJob failed validation (anim tracks={}, context max={}, output soa={})",
					anim->data->num_tracks(), samplingContext.max_tracks(), localPose.size());
			}
			return;
		}
		UnpackSoaTransforms({ localPose.data(), localPose.size() }, { outputPose.data(), outputPose.size() }, skeleton->data.get());
		hasPose = true;

		auto* rig = a_modelNode->rig;
		if (!rig || !rig->local || !rig->local->data) {
			return;
		}
		float* buf = reinterpret_cast<float*>(rig->local->data);

		// Bound every write to the LIVE buffer. binding.rigIndex was validated when the binding was built, but the rig can be rebuilt (smaller) between bind and stamp on the anim job thread;
		const uint16_t rigBoneCount = GetRigBoneCount(a_modelNode);

		if (poseMode == PoseMode::kAdditive) {
			if (liveBasePose.size() != binding.size() * 16) {
				return;  // binding/base must be prepared together; never allocate from the stamp hook
			}
			// AnimationManager::Update evaluates Starfield first and increments enginePoseRevision via
			// Sample. Capture those fresh local slots once. If BGSModelNode::Update repeats before a new
			// engine evaluation, keep this immutable base instead of reading our prior additive write.
			if (basePoseRevision != enginePoseRevision) {
				for (std::size_t i = 0; i < binding.size(); ++i) {
					const auto rigIdx = binding[i].first;
					if (rigIdx < rigBoneCount) {
						std::memcpy(liveBasePose.data() + i * 16,
							buf + static_cast<std::size_t>(rigIdx) * 16, 16 * sizeof(float));
					}
				}
				basePoseRevision = enginePoseRevision;
			}
			const float effectiveWeight = PoseMath::EffectiveWeight(weight, poseWeight);
			for (std::size_t i = 0; i < binding.size(); ++i) {
				const auto [rigIdx, jointIdx] = binding[i];
				if (rigIdx >= rigBoneCount) {
					continue;
				}
				float* slot = buf + static_cast<std::size_t>(rigIdx) * 16;
				PoseMath::WriteAdditive(slot, liveBasePose.data() + i * 16,
					reinterpret_cast<const float*>(&referencePose[jointIdx]),
					reinterpret_cast<const float*>(&outputPose[jointIdx]), effectiveWeight);
			}
		} else if (weight >= 1.0f) {
			for (const auto& [rigIdx, jointIdx] : binding) {
				if (rigIdx >= rigBoneCount) {
					continue;
				}
				WriteNiTransformRows(buf + static_cast<size_t>(rigIdx) * 16, outputPose[jointIdx]);
			}
		} else {
			const bool fromSnapshot = blendPhase == BlendPhase::kIn && blendFromValid;
			for (const auto& [rigIdx, jointIdx] : binding) {
				if (rigIdx >= rigBoneCount) {
					continue;
				}
				float* slot = buf + static_cast<size_t>(rigIdx) * 16;
				const float* from = fromSnapshot ?
				                        reinterpret_cast<const float*>(&blendFromPose[jointIdx]) :
				                        slot;  // engine's live pose this frame
				WriteNiTransformRowsBlended(slot, from, outputPose[jointIdx], weight);
			}
		}

		if (!loggedFirstApply) {
			loggedFirstApply = true;
			REX::TRACE("[Anim] first pose stamped pre-compose ({} bones)", binding.size());
		}
	}
}
