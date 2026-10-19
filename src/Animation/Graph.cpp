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

		// Body only: face (faceBone_*), morphs and the eye look-at bones stay engine-driven. SAF/NAF clips
		// author eye bones with ~1m look-space translations that fling the eyes off the head under C_Head.
		bool IsNonBodyRigNode(std::string_view a_lowerName)
		{
			return a_lowerName.starts_with("facebone") || a_lowerName.starts_with("morph") ||
			       a_lowerName == "l_eye" || a_lowerName == "r_eye" || a_lowerName == "eye_target";
		}

		// NiTransform slot: 3 rotation rows of 4 floats, translation +0x30, scale +0x3C (byte-identical to ozz column-major).
		// BGSModelNode+0x78 = u16 rigBoneCount: element count of rig->local/world/prevWorld.
		constexpr std::uintptr_t kModelNodeRigBoneCountOffset = 0x78;
		inline uint16_t GetRigBoneCount(const RE::BGSModelNode* a_modelNode)
		{
			return *reinterpret_cast<const uint16_t*>(
				reinterpret_cast<std::uintptr_t>(a_modelNode) + kModelNodeRigBoneCountOffset);
		}

	}

	void Graph::PrepareBlendSource()
	{
		if (!hasPose) {
			preparedBlendSource.Clear();
			return;
		}
		preparedBlendSource.Capture(displayedPose, displayedDriven);
	}

	void Graph::SetAnimation(std::shared_ptr<const OzzSkeleton> a_skeleton, std::shared_ptr<const OzzAnimation> a_anim, std::string a_file)
	{
		if (++playbackRevision == 0) {
			++playbackRevision;  // reserve zero for a graph that has never started
		}

		const auto nextJointCount = static_cast<std::size_t>(a_skeleton->data->num_joints());
		bool sameJointLayout = skeleton == a_skeleton;
		if (!sameJointLayout && skeleton && skeleton->data->num_joints() == a_skeleton->data->num_joints()) {
			const auto oldNames = skeleton->data->joint_names();
			const auto newNames = a_skeleton->data->joint_names();
			sameJointLayout = std::equal(oldNames.begin(), oldNames.end(), newNames.begin(),
				[](const char* oldName, const char* newName) { return std::strcmp(oldName, newName) == 0; });
		}
		if (!sameJointLayout) {
			preparedBlendSource.Clear();  // joint indices must retain their meaning, not just their count
		}
		// Crossfade from the on-screen pose: prepared before teardown on replacement, else captured live.
		if (preparedBlendSource.Consume(nextJointCount, blendFromPose, blendFromDriven)) {
			blendFromValid = true;
		} else if (sameJointLayout && hasPose && displayedPose.size() == nextJointCount) {
			blendFromPose = displayedPose;
			blendFromDriven = displayedDriven;
			blendFromValid = true;
		} else {
			blendFromPose.clear();
			blendFromDriven.clear();
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
		hasPose = blendFromValid;
		holdClipAtEnd = false;
		if (!playbackSession) {
			syncGroup = std::make_shared<SyncGroup>();	// new clip owns the clock if not in a scene
		}

		const int numJoints = static_cast<int>(nextJointCount);
		samplingContext.Resize(numJoints);
		localPose.resize(skeleton->data->num_soa_joints());
		outputPose.assign(numJoints, ozz::math::Float4x4::identity());
		if (blendFromValid) {
			// Two commands can replace playback before any compose. The last displayed pose is still
			// the handoff source in that case, not this new clip's as-yet-unsampled first frame.
			displayedPose = blendFromPose;
			displayedDriven = blendFromDriven;
		} else {
			displayedPose.assign(numJoints, ozz::math::Float4x4::identity());
			displayedDriven.assign(numJoints, 0);
		}
		referencePose.assign(numJoints, ozz::math::Float4x4::identity());
		rigIndexByJoint.assign(numJoints, UINT16_MAX);
		const auto restPose = skeleton->data->joint_rest_poses();
		UnpackSoaTransforms({ restPose.data(), restPose.size() },
			{ referencePose.data(), referencePose.size() }, skeleton->data.get());

		jointMap.clear();
		auto jointNames = skeleton->data->joint_names();
		for (size_t i = 0; i < jointNames.size(); i++) {
			jointMap[ToLower(jointNames[i])] = static_cast<uint16_t>(i);
		}

		InvalidateBinding(sameJointLayout);
	}

	void Graph::InvalidateBinding(bool a_preserveLiveBase)
	{
		cachedModelNode.store(nullptr, std::memory_order_relaxed);
		cachedRig = nullptr;
		cachedBoneCount = 0;
		cachedLocalData = nullptr;
		cachedRigBoneCount = 0;
		binding.clear();
		liveBasePose.clear();
		liveBaseValid = false;
		basePoseRevision = 0;
		evaluatedBinding.Clear();
		stampProbeValid = false;
		if (!a_preserveLiveBase) {
			liveBaseCache.Clear();
		}
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

		// The binding is the write policy; rebuild even if the rig is unchanged.
		InvalidateBinding(true);
	}

	void Graph::SetBoneMask(const std::string& a_maskName)
	{
		const BoneMask::Mask* next = a_maskName.empty() ? nullptr : BoneMask::Find(a_maskName);
		if (!a_maskName.empty() && !next) {
			REX::WARN("[Anim] unknown bone mask '{}' — playing unmasked (known: {})",
				a_maskName, BoneMask::KnownList());
		}
		if (next == boneMask) {
			return;
		}
		boneMask = next;
		loggedMaskBind = false;
		InvalidateBinding(true);
	}

	bool Graph::ResolveAndBind(const RE::BGSModelNode* a_expectedModelNode)
	{
		// cachedModelNode is the stamp hook's match key; once the chain stops resolving the address may be reused.
		const auto fail = [this, a_expectedModelNode]() {
			cachedModelNode.store(nullptr, std::memory_order_relaxed);
			cachedRig = nullptr;
			cachedBoneCount = 0;
			cachedLocalData = nullptr;
			cachedRigBoneCount = 0;
			binding.clear();
			liveBasePose.clear();
			liveBaseValid = false;
			basePoseRevision = 0;
			evaluatedBinding.Clear();
			stampProbeValid = false;
			// Compose hook expected this node: composes run unstamped (a visible vanilla-pose frame).
			if (a_expectedModelNode) {
				composeResolveFailCount++;
				if (composeResolveFailCount == 1 || (composeResolveFailCount & 255u) == 0) {
					REX::TRACE("[Anim] compose resolve FAILED — actor {:08X} unstamped this compose (#{}, blendPhase {})",
						target ? target->formID : 0, composeResolveFailCount, static_cast<int>(blendPhase));
				}
			}
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
		if (a_expectedModelNode && modelNode != a_expectedModelNode) {
			// Another skeleton hit the game-wide compose hook while we were unbound; wait for ours.
			return false;
		}

		auto* rig = modelNode->rig;
		if (!rig || !rig->local || !rig->local->data) {
			return fail();
		}

		const auto* previousModelNode = cachedModelNode.load(std::memory_order_relaxed);
		// Identity includes the buffer base and rigBoneCount: a modelNode reused at the same address with a
		// smaller buffer would otherwise keep rig indices past its end.
		if (modelNode == previousModelNode && rig == cachedRig
			&& rig->local->data == cachedLocalData
			&& modelNode->nodes.size() == cachedBoneCount
			&& GetRigBoneCount(modelNode) == cachedRigBoneCount) {
			return !binding.empty();
		}

		// Different modelNode = 3D rebuilt under us (e.g. equipment restore at scene end).
		if (previousModelNode && previousModelNode != modelNode) {
			REX::TRACE("[Anim] rig REBIND — modelNode {} -> {} (3d rebuilt; blendPhase {})",
				static_cast<const void*>(previousModelNode), static_cast<const void*>(modelNode),
				static_cast<int>(blendPhase));
		}
		// Same node, changed identity = node table / rig buffer mutated in place (helmet, props).
		if (previousModelNode == modelNode && loggedBind) {
			REX::TRACE("[Anim] rig re-bind — same modelNode, identity changed (nodes {} -> {}, rigBones {} -> {})",
				cachedBoneCount, modelNode->nodes.size(), cachedRigBoneCount, GetRigBoneCount(modelNode));
		}

		cachedModelNode.store(modelNode, std::memory_order_relaxed);
		cachedRig = rig;
		cachedBoneCount = modelNode->nodes.size();
		cachedLocalData = rig->local->data;
		cachedRigBoneCount = GetRigBoneCount(modelNode);
		binding.clear();
		binding.reserve(cachedBoneCount);
		std::fill(rigIndexByJoint.begin(), rigIndexByJoint.end(), UINT16_MAX);

		uint32_t skippedNonBody = 0;
		uint32_t skippedPreserved = 0;
		uint32_t skippedMasked = 0;
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
			if (auto iter = jointMap.find(lowerName); iter != jointMap.end() && entry.rigIndex < cachedRigBoneCount) {
				rigIndexByJoint[iter->second] = entry.rigIndex;
			}
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
				float boneWeight = 1.0f;
				if (boneMask) {
					const auto mit = boneMask->weights.find(lowerName);
					if (mit == boneMask->weights.end()) {
						skippedMasked++;
						boneWeight = 0.0f;
					} else {
						boneWeight = mit->second;
					}
				}
				// Outgoing-only bones remain bound during the crossfade so a narrower mask releases
				// them to vanilla over the blend, rather than dropping the torso/head in one frame.
				const bool retiring = poseMode == PoseMode::kOverride && blendPhase == BlendPhase::kIn &&
					blendFromValid && iter->second < blendFromDriven.size() && blendFromDriven[iter->second];
				if (boneWeight <= 0.0f && !retiring) {
					continue;
				}
				binding.push_back({ entry.rigIndex, iter->second, boneWeight });
			}
		}
		if (++bindingRevision == 0) {
			++bindingRevision;
		}
		// Seed the live base from the last proven base by skeleton joint; a compose-time rebind may run
		// before the engine evaluates the new rig.
		liveBasePose.assign(binding.size() * 16, 0.0f);
		liveBaseValid = liveBaseCache.Restore(
			std::span{ binding }, std::span{ liveBasePose });
		basePoseRevision = 0;
		evaluatedBinding.Clear();
		stampProbeValid = false;

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
		if (boneMask && !loggedMaskBind) {
			loggedMaskBind = true;
			REX::DEBUG("[Anim] bone mask '{}' — actor {:08X}, role '{}': {} driven bone(s){}, {} left engine-driven",
				boneMask->id, target ? target->formID : 0, roleName.empty() ? "<anonymous>" : roleName,
				binding.size(), boneMask->feathered ? " (feathered seam)" : "", skippedMasked);
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
		// Drop the node cache: if sampling stops mid-fade, a stale cachedModelNode could match a reused
		// address and stamp a foreign rig. A live fade re-binds on the next Sample.
		InvalidateBinding(true);
	}

	void Graph::DetachAndFadeOut()
	{
		playbackSession = nullptr;
		participantIndex = -1;
		syncGroup = std::make_shared<SyncGroup>();  // solo group of 1
		syncGroup->clock.time = localTime;

		// Hand C_Head back on the first teardown frame: blending it through the fade lets facial/equipment
		// rebuilds cache a half-OSF head pose (stuck on NPCs). BeginFadeOut's rebind picks this up.
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
			++enginePoseRevision;
		}
		lastSampleMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);

		if (blendPhase != BlendPhase::kNone && blendClock.ShouldAdvance(a_token)) {
			blendClock.time += a_deltaTime;
		}

		// A segment switch clears the binding, so the new clip binds again in this call.
		if (playbackSession) {
			const auto tick = playbackSession->Advance(a_token, a_deltaTime);
			if (tick.stage != appliedStage && participantIndex >= 0 && tick.stage < playbackSession->stages.size()) {
				const auto& stage = playbackSession->stages[tick.stage];
				const auto& slot = stage.participants[participantIndex];
				SetAnimation(slot.skeleton, slot.anim, slot.file);
				SetPosePolicy(stage.poseModes[participantIndex], stage.poseWeights[participantIndex], roleName);
				SetBoneMask(stage.masks[participantIndex]);
				blendDuration = stage.blendIn;
				scenePlacement = AddPlacement(stage.placements[participantIndex], alignDelta);
				appliedStage = tick.stage;
			}
			localTime = tick.time;
			// Sticks through DetachAndFadeOut so the fade-out also samples the end pose.
			holdClipAtEnd = tick.holdEnd;
			// Bound the sample to this participant's clip rather than the stage's reference duration.
			if (anim && anim->data && anim->data->duration() > 0.0f) {
				localTime = tick.SampleTime(anim->data->duration());
			}
		}

		if (!anim || !skeleton) {
			return;
		}

		if (!ResolveAndBind()) {
			return;
		}
		// Sample runs only after the engine evaluated this actor, authorizing capture for this binding generation.
		evaluatedBinding.Mark(enginePoseRevision, bindingRevision);

		const float duration = anim->data->duration();
		if (duration <= 0.0f) {
			return;
		}

		// Only the owner token advances solo time (e.g. 1st/3rd person graphs both report).
		if (!playbackSession && syncGroup) {
			std::scoped_lock sgl{ syncGroup->lock };
			auto& clk = syncGroup->clock;
			const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
			if (clk.ShouldAdvance(a_token, nowMs)) {
				clk.time += a_deltaTime * syncGroup->speed.load(std::memory_order_relaxed);
			}
			if (holdClipAtEnd) {
				localTime = std::min(clk.time, duration);
			} else {
				float t = clk.time;
				t = std::fmod(t, duration);
				if (t < 0.0f) {
					t += duration;
				}
				localTime = t;
			}
		}

		// Sampling is deferred to StampPose (once per frame); this only advances time/stage/cues and keeps
		// the binding warm.
	}

	void Graph::StampPose(const RE::BGSModelNode* a_modelNode)
	{
		if (a_modelNode != cachedModelNode.load(std::memory_order_relaxed) ||
			binding.empty() || !anim || !skeleton ||
			outputPose.size() != referencePose.size()) {
			return;
		}
		// Helmet/prop patches mutate the node table (and may remap rig slots) at the same address before
		// Sample notices; stamping the old binding would scramble the frame, so re-bind now.
		{
			auto* liveRig = a_modelNode->rig;
			const bool stale = !liveRig || !liveRig->local || liveRig->local->data != cachedLocalData ||
			                   a_modelNode->nodes.size() != cachedBoneCount ||
			                   GetRigBoneCount(a_modelNode) != cachedRigBoneCount;
			if (stale) {
				const auto prevNodes = cachedBoneCount;
				staleStampRebindCount++;
				if (!ResolveAndBind(a_modelNode)) {
					REX::TRACE("[Anim] stamp SKIPPED — node-table mutation and re-bind failed (actor {:08X}, mutation #{}, nodes {} -> {})",
						target ? target->formID : 0, staleStampRebindCount, prevNodes, a_modelNode->nodes.size());
					return;
				}
				REX::TRACE("[Anim] compose re-bound after node-table mutation — actor {:08X}, mutation #{}, nodes {} -> {}, {} driven bone(s)",
					target ? target->formID : 0, staleStampRebindCount, prevNodes, cachedBoneCount, binding.size());
			}
		}

		// Slots hold the engine's live pose for this frame, so blends are against what the actor would do.
		float weight = 1.0f;
		if (blendPhase == BlendPhase::kIn) {
			weight = blendDuration > 0.0f ? std::min(blendClock.time / blendDuration, 1.0f) : 1.0f;
		} else if (blendPhase == BlendPhase::kOut) {
			weight = blendDuration > 0.0f ? 1.0f - std::min(blendClock.time / blendDuration, 1.0f) : 0.0f;
			if (weight <= 0.0f) {
				return;
			}
		}
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

		auto* rig = a_modelNode->rig;
		if (!rig || !rig->local || !rig->local->data) {
			return;
		}
		float* buf = reinterpret_cast<float*>(rig->local->data);

		// Bound every write: the rig can shrink between bind and stamp.
		const uint16_t rigBoneCount = GetRigBoneCount(a_modelNode);
		// Capture engine-evaluated slots once per enginePoseRevision; repeated composes reuse that base rather
		// than read our own write (additive and sub-unity override weights would converge on themselves).
		const auto captureLiveBase = [&]() -> bool {
			if (liveBasePose.size() != binding.size() * 16) {
				return false;  // never allocate from the stamp hook
			}
			if (basePoseRevision != enginePoseRevision) {
				if (!evaluatedBinding.IsCurrent(enginePoseRevision, bindingRevision)) {
					if (!liveBaseValid) {
						return false;
					}
					basePoseRevision = enginePoseRevision;
					rebindBaseCarryCount++;
					if (rebindBaseCarryCount == 1 || (rebindBaseCarryCount & 63u) == 0) {
						REX::TRACE("[Anim] live-base capture deferred — binding has no post-engine evaluation; reusing prior base (actor {:08X}, #{})",
							target ? target->formID : 0, rebindBaseCarryCount);
					}
					return true;
				}
				// If the probe slot still holds our last write, the engine hasn't re-applied its pose; adopting it
				// would poison the base (spine pops). A handoff can have stamped a held snapshot
				// before the first capture, so the probe also gates that first capture.
				if (stampProbeValid && stampProbeRigIdx < rigBoneCount &&
					std::memcmp(buf + static_cast<std::size_t>(stampProbeRigIdx) * 16,
						stampProbe.data(), 16 * sizeof(float)) == 0) {
					if (liveBaseValid) {
						basePoseRevision = enginePoseRevision;
					}
					captureGateCount++;
					if (captureGateCount == 1 || (captureGateCount & 63u) == 0) {
						REX::TRACE("[Anim] live-base capture gated — buffer unchanged since our stamp (actor {:08X}, #{})",
							target ? target->formID : 0, captureGateCount);
					}
					return liveBaseValid;
				}
				bool capturedAll = true;
				for (std::size_t i = 0; i < binding.size(); ++i) {
					const auto rigIdx = binding[i].rigIndex;
					if (rigIdx < rigBoneCount) {
						std::memcpy(liveBasePose.data() + i * 16,
							buf + static_cast<std::size_t>(rigIdx) * 16, 16 * sizeof(float));
					} else {
						capturedAll = false;
					}
				}
				if (!capturedAll) {
					liveBaseValid = false;
					return false;
				}
				liveBaseValid = true;
				liveBaseCache.Store(std::span{ binding }, std::span{ liveBasePose },
					outputPose.size());
				basePoseRevision = enginePoseRevision;
			}
			return liveBaseValid;
		};

		// Probe for the next capture: the bytes we leave in the first written slot.
		bool probeRecorded = false;
		const auto recordProbe = [&](const float* a_slot, uint16_t a_rigIdx) {
			std::memcpy(stampProbe.data(), a_slot, 16 * sizeof(float));
			stampProbeRigIdx = a_rigIdx;
			stampProbeValid = true;
			probeRecorded = true;
		};
		std::fill(displayedDriven.begin(), displayedDriven.end(), 0);
		hasPose = false;
		const auto recordDisplay = [&](const float* a_slot, const BoundBone& a_bound) {
			std::memcpy(&displayedPose[a_bound.jointIndex], a_slot, 16 * sizeof(float));
			displayedDriven[a_bound.jointIndex] = 1;
			hasPose = true;
			if (!probeRecorded) {
				recordProbe(a_slot, a_bound.rigIndex);
			}
		};

		if (poseMode == PoseMode::kAdditive) {
			if (!captureLiveBase()) {
				return;
			}
			const float effectiveWeight = PoseMath::EffectiveWeight(weight, poseWeight);
			for (std::size_t i = 0; i < binding.size(); ++i) {
				const auto& bound = binding[i];
				if (bound.rigIndex >= rigBoneCount) {
					continue;
				}
				float* slot = buf + static_cast<std::size_t>(bound.rigIndex) * 16;
				PoseMath::WriteAdditive(slot, liveBasePose.data() + i * 16,
					reinterpret_cast<const float*>(&referencePose[bound.jointIndex]),
					reinterpret_cast<const float*>(&outputPose[bound.jointIndex]),
					effectiveWeight * bound.weight);
				if (effectiveWeight * bound.weight > 0.0f) {
					recordDisplay(slot, bound);
				}
			}
		} else {
			// Capture before any writes. If a rebind precedes the next engine evaluation, each outgoing
			// joint can still hold its displayed snapshot; newly included joints wait for a proven base.
			const bool haveBase = captureLiveBase();
			const bool hasSnapshot = blendPhase == BlendPhase::kIn && blendFromValid;
			for (std::size_t i = 0; i < binding.size(); ++i) {
				const auto& bound = binding[i];
				if (bound.rigIndex >= rigBoneCount) {
					continue;
				}
				float* slot = buf + static_cast<std::size_t>(bound.rigIndex) * 16;
				// Only joints the outgoing stage stamped have a valid snapshot; newly masked-in joints blend from live.
				const bool fromSnapshot = hasSnapshot && bound.jointIndex < blendFromDriven.size() &&
				                          blendFromDriven[bound.jointIndex] != 0;
				const float strength = poseWeight * bound.weight;
				if (!PoseMath::WriteOverrideTransition(slot,
					haveBase ? liveBasePose.data() + i * 16 : nullptr,
					reinterpret_cast<const float*>(&outputPose[bound.jointIndex]),
					fromSnapshot ? reinterpret_cast<const float*>(&blendFromPose[bound.jointIndex]) : nullptr,
					weight, strength)) {
					continue;
				}
				if (strength > 0.0f || (fromSnapshot && (weight < 1.0f || !haveBase))) {
					recordDisplay(slot, bound);
				} else if (!probeRecorded) {
					recordProbe(slot, bound.rigIndex);
				}
			}
		}
		if (!probeRecorded) {
			stampProbeValid = false;
		}
		if (blendPhase == BlendPhase::kIn && weight >= 1.0f && liveBaseValid) {
			blendPhase = BlendPhase::kNone;
		}

		if (!loggedFirstApply) {
			loggedFirstApply = true;
			REX::TRACE("[Anim] first pose stamped pre-compose ({} bones)", binding.size());
		}
	}
}
