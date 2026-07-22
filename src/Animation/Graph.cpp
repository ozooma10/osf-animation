#include "Graph.h"

#include "Animation/Scene.h"
#include "Util/StringUtil.h"

#include "ozz/base/span.h"

#include <chrono>

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

		// --- cross-fade math ---
		// Largely ai slop "MAKE THIS WORK PLZ"

		struct Quat
		{
			float w, x, y, z;
		};

		// a_m = 16 floats; 3x3 at [0..2]/[4..6]/[8..10], M(r,c) = a_m[c*4 + r]
		Quat MatrixToQuat(const float* a_m)
		{
			const float m00 = a_m[0], m10 = a_m[1], m20 = a_m[2];
			const float m01 = a_m[4], m11 = a_m[5], m21 = a_m[6];
			const float m02 = a_m[8], m12 = a_m[9], m22 = a_m[10];
			const float trace = m00 + m11 + m22;
			Quat q;
			if (trace > 0.0f) {
				const float s = std::sqrt(trace + 1.0f) * 2.0f;
				q.w = 0.25f * s;
				q.x = (m21 - m12) / s;
				q.y = (m02 - m20) / s;
				q.z = (m10 - m01) / s;
			} else if (m00 > m11 && m00 > m22) {
				const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
				q.w = (m21 - m12) / s;
				q.x = 0.25f * s;
				q.y = (m01 + m10) / s;
				q.z = (m02 + m20) / s;
			} else if (m11 > m22) {
				const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
				q.w = (m02 - m20) / s;
				q.x = (m01 + m10) / s;
				q.y = 0.25f * s;
				q.z = (m12 + m21) / s;
			} else {
				const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
				q.w = (m10 - m01) / s;
				q.x = (m02 + m20) / s;
				q.y = (m12 + m21) / s;
				q.z = 0.25f * s;
			}
			return q;
		}

		void QuatToMatrix3x3(const Quat& a_q, float* a_m)
		{
			const float xx = a_q.x * a_q.x, yy = a_q.y * a_q.y, zz = a_q.z * a_q.z;
			const float xy = a_q.x * a_q.y, xz = a_q.x * a_q.z, yz = a_q.y * a_q.z;
			const float wx = a_q.w * a_q.x, wy = a_q.w * a_q.y, wz = a_q.w * a_q.z;
			a_m[0] = 1.0f - 2.0f * (yy + zz);   // m00
			a_m[1] = 2.0f * (xy + wz);          // m10
			a_m[2] = 2.0f * (xz - wy);          // m20
			a_m[4] = 2.0f * (xy - wz);          // m01
			a_m[5] = 1.0f - 2.0f * (xx + zz);   // m11
			a_m[6] = 2.0f * (yz + wx);          // m21
			a_m[8] = 2.0f * (xz + wy);          // m02
			a_m[9] = 2.0f * (yz - wx);          // m12
			a_m[10] = 1.0f - 2.0f * (xx + yy);  // m22
		}

		Quat Nlerp(const Quat& a_from, Quat a_to, float a_t)
		{
			const float dot = a_from.w * a_to.w + a_from.x * a_to.x + a_from.y * a_to.y + a_from.z * a_to.z;
			if (dot < 0.0f) {  // shortest arc
				a_to = { -a_to.w, -a_to.x, -a_to.y, -a_to.z };
			}
			Quat q{
				a_from.w + (a_to.w - a_from.w) * a_t,
				a_from.x + (a_to.x - a_from.x) * a_t,
				a_from.y + (a_to.y - a_from.y) * a_t,
				a_from.z + (a_to.z - a_from.z) * a_t
			};
			const float lenSq = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
			if (lenSq > 1e-8f) {
				const float inv = 1.0f / std::sqrt(lenSq);
				q = { q.w * inv, q.x * inv, q.y * inv, q.z * inv };
			} else {
				q = { 1.0f, 0.0f, 0.0f, 0.0f };
			}
			return q;
		}

		void WriteNiTransformRowsBlended(float* a_slot, const float* a_from, const ozz::math::Float4x4& a_target, float a_weight)
		{
			const float* to = reinterpret_cast<const float*>(&a_target);
			const Quat q = Nlerp(MatrixToQuat(a_from), MatrixToQuat(to), a_weight);
			const float tx = a_from[12] + (to[12] - a_from[12]) * a_weight;
			const float ty = a_from[13] + (to[13] - a_from[13]) * a_weight;
			const float tz = a_from[14] + (to[14] - a_from[14]) * a_weight;
			QuatToMatrix3x3(q, a_slot);
			a_slot[3] = a_slot[7] = a_slot[11] = 0.0f;
			a_slot[12] = tx;
			a_slot[13] = ty;
			a_slot[14] = tz;
			a_slot[15] = 1.0f;
		}
	}

	void Graph::SetAnimation(std::shared_ptr<const OzzSkeleton> a_skeleton, std::shared_ptr<const OzzAnimation> a_anim, std::string a_file)
	{
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

		if (!loggedBind) {
			loggedBind = true;
			REX::DEBUG("[Anim] rig bind — {}/{} mapped body bones matched skeleton joints ({} face/eye/morph nodes skipped)", binding.size(), cachedBoneCount, skippedNonBody);
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
	}

	bool Graph::IsFadedOut() const
	{
		return blendPhase == BlendPhase::kOut && blendClock.time >= blendDuration;
	}

	void Graph::Sample(float a_deltaTime, const void* a_token)
	{
		// Blend ramps run on playback dt, independent of clip looping/clamping
		if (blendPhase != BlendPhase::kNone && blendClock.ShouldAdvance(a_token)) {
			blendClock.time += a_deltaTime;
		}

		// Advance scene clock first, if it triggers stage switch (new anim), clears rig binding, which will cause below to rebind in same call.
		if (scene) {
			const auto tick = scene->Advance(a_token, a_deltaTime);
			if (tick.stage != appliedStage && participantIndex >= 0 && tick.stage < scene->stages.size()) {
				const auto& slot = scene->stages[tick.stage].participants[participantIndex];
				SetAnimation(slot.skeleton, slot.anim, slot.file);
				blendDuration = scene->stages[tick.stage].blendIn;  // per-stage blend-in
				appliedStage = tick.stage;
			}
			localTime = tick.time;
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
			//if owner clock stoped, elect new owner.
			if (clk.owner && a_token != clk.owner && nowMs - syncGroup->lastAdvanceMs > kSyncOwnerStaleMs) {
				clk.owner = nullptr;
			}
			if (clk.ShouldAdvance(a_token)) {
				clk.time += a_deltaTime * syncGroup->speed.load(std::memory_order_relaxed);
				syncGroup->lastAdvanceMs = nowMs;
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
		if (a_modelNode != cachedModelNode || binding.empty() || !anim || !skeleton) {
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

		if (weight >= 1.0f) {
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
