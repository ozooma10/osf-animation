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

		bool IsNameSeparator(char a_char)
		{
			return !std::isalnum(static_cast<unsigned char>(a_char));
		}

		bool ContainsDelimitedToken(std::string_view a_name, std::string_view a_token)
		{
			size_t pos = a_name.find(a_token);
			while (pos != std::string_view::npos) {
				const bool leftOk = pos == 0 || IsNameSeparator(a_name[pos - 1]);
				const size_t right = pos + a_token.size();
				const bool rightOk = right >= a_name.size() || IsNameSeparator(a_name[right]);
				if (leftOk && rightOk) {
					return true;
				}
				pos = a_name.find(a_token, pos + 1);
			}
			return false;
		}

		bool IsFaceRigNode(std::string_view a_lowerName)
		{
			// Director drives body playback only. NAF-lineage GLBs are bones-only
			// today, but future imports may carry expression tracks whose names
			// collide with Starfield's facial rig. Keep head/neck structural
			// joints bindable; explicitly leave facial controls to the engine.
			return a_lowerName.starts_with("face") || a_lowerName.starts_with("facial") ||
			       a_lowerName.starts_with("morph") || ContainsDelimitedToken(a_lowerName, "facial") ||
			       ContainsDelimitedToken(a_lowerName, "eye") || ContainsDelimitedToken(a_lowerName, "eyelid") ||
			       ContainsDelimitedToken(a_lowerName, "eyelash") || ContainsDelimitedToken(a_lowerName, "brow") ||
			       ContainsDelimitedToken(a_lowerName, "cheek") || ContainsDelimitedToken(a_lowerName, "jaw") ||
			       ContainsDelimitedToken(a_lowerName, "lip") || ContainsDelimitedToken(a_lowerName, "mouth") ||
			       ContainsDelimitedToken(a_lowerName, "nose") || ContainsDelimitedToken(a_lowerName, "teeth") ||
			       ContainsDelimitedToken(a_lowerName, "tongue") || ContainsDelimitedToken(a_lowerName, "ear");
		}

		// Write one bone slot in the engine's NiTransform layout: rotation as
		// 3 rows of 4 floats (+0x00/+0x10/+0x20), translation +0x30, uniform
		// scale +0x3C. CONVENTION (settled by the engine's own quat→matrix
		// routine 0x1404001C0, sign pattern R01=2xy+2wz / R10=2xy-2wz):
		// Bethesda rotations are row-vector convention, so the stored rows are
		// the TRANSPOSE of the standard column-vector matrix — which makes them
		// byte-identical to ozz's column-major storage. A straight copy is
		// correct; do NOT transpose (a transposed write inverts every bone
		// rotation — live-observed as a fully contorted rig).
		void WriteNiTransformRows(float* a_slot, const ozz::math::Float4x4& a_matrix)
		{
			const float* m = reinterpret_cast<const float*>(&a_matrix);
			std::memcpy(a_slot, m, 60);  // 3 basis columns (= engine rows) + translation
			a_slot[15] = 1.0f;           // uniform scale
		}

		// --- cross-fade math -------------------------------------------------
		// Both blend sources share the slot/ozz storage convention, so any
		// self-consistent matrix<->quat pair is correct here (a transposed
		// reading would yield conjugate quats, and nlerp of conjugates is the
		// conjugate of the nlerp — the round trip cancels).

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

		// Blend a_from (slot layout; may alias a_slot — all reads happen before
		// any write) toward a_target by a_weight and write the slot: quaternion
		// nlerp for rotation, lerp for translation, scale forced to 1.
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
		// Cross-fade FROM the pose currently on screen when there is one and
		// the joint indexing carries over (same rig family — true for stage
		// switches and replays); otherwise blend in from the engine's live
		// pose, read per-bone at the write point.
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
		soloClock.Reset();

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
		binding.clear();
	}

	bool Graph::ResolveAndBind()
	{
		// Any failure below invalidates the binding cache, not just the return:
		// cachedModelNode is the stamp hook's match key, and once the chain
		// stops resolving (3D detached, e.g. during a save-load teardown) the
		// cached address can be freed and reused by an unrelated BGSModelNode —
		// a stale match would stamp an old binding into the wrong rig.
		const auto fail = [this]() {
			lastRoot = nullptr;
			cachedModelNode = nullptr;
			cachedRig = nullptr;
			cachedBoneCount = 0;
			binding.clear();
			return false;
		};

		auto* refr = target.get();
		if (!refr) {
			return fail();
		}

		// refr -> data3D -> BSFadeNode -> BGSModelNode -> rig -> local buffer.
		// Re-resolved every call: all of these churn on reload/rig rebuild.
		// The root ref is taken UNDER the loadedData lock and held as a
		// NiPointer for everything below: during a save-load the engine tears
		// the 3D down on other threads while this graph is still live (the
		// teardown event fires only at the load finalizer), so a raw pointer
		// used after the lock is released is a use-after-free.
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
		lastRoot = root;

		auto* modelNode = root->bgsModelNode.get();
		if (!modelNode) {
			return fail();
		}
		auto* rig = modelNode->rig;
		if (!rig || !rig->local || !rig->local->data) {
			return fail();
		}

		if (modelNode == cachedModelNode && rig == cachedRig && modelNode->nodes.size() == cachedBoneCount) {
			return !binding.empty();
		}

		// (re)build the rigIndex -> jointIndex binding from the bone map
		cachedModelNode = modelNode;
		cachedRig = rig;
		cachedBoneCount = modelNode->nodes.size();
		binding.clear();
		binding.reserve(cachedBoneCount);

		uint32_t skippedFaceBones = 0;
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
			if (IsFaceRigNode(lowerName)) {
				skippedFaceBones++;
				continue;
			}
			if (auto iter = jointMap.find(lowerName); iter != jointMap.end()) {
				binding.emplace_back(entry.rigIndex, iter->second);
			}
		}

		if (!loggedBind) {
			loggedBind = true;
			REX::INFO("Graph: rig bind — {}/{} mapped body bones matched skeleton joints ({} face nodes skipped)",
				binding.size(), cachedBoneCount, skippedFaceBones);
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
		// Solo-graph stall heartbeat: refreshed on EVERY update call (any token,
		// even dt 0 during a menu pause), so it only goes quiet when this graph's
		// AnimationManager stops pumping entirely — i.e. the actor unloaded.
		// GraphManager::WatchdogSweep reaps the graph when this stays stale.
		lastSampleMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);

		// Blend ramps run on the sanitized playback dt (owner-token gated),
		// independent of clip looping/clamping; dt 0 holds the ramp.
		if (blendPhase != BlendPhase::kNone && blendClock.ShouldAdvance(a_token)) {
			blendClock.time += a_deltaTime;
		}

		// Advance the scene clock first: it owns the stage, and a stage switch
		// swaps this graph's animation (preloaded slot, no IO) which clears the
		// rig binding — ResolveAndBind below then rebinds in the same call.
		// scene->stages is immutable after publish, safe to read lock-free.
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

		// Solo: our own owner-token clock advances time (only the first
		// AnimationManager seen advances, so a graph updated by two managers —
		// e.g. the player's 1st/3rd person graphs — runs at 1x regardless of
		// interleaving; in a scene the shared clock above does the same job).
		if (!scene) {
			if (syncGroup) {
				// Synced group: a shared free-running accumulator owned by the
				// first reporting manager; the rest read it. Wrapped by THIS
				// graph's own duration so mismatched clips still loop, and advanced
				// by the GROUP speed (any member's SetSpeed sets it).
				std::scoped_lock sgl{ syncGroup->lock };
				auto& clk = syncGroup->clock;
				const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
				// Re-elect the owner if the current one has gone silent (its graph
				// stopped or its actor unloaded); without this the dead owner never
				// advances again and the whole group freezes (solo graphs have no
				// WatchdogSweep). The next ShouldAdvance below claims the clock.
				if (clk.owner && a_token != clk.owner && nowMs - syncGroup->lastAdvanceMs > kSyncOwnerStaleMs) {
					clk.owner = nullptr;
				}
				if (clk.ShouldAdvance(a_token)) {
					clk.time += a_deltaTime * syncGroup->speed.load(std::memory_order_relaxed);
					syncGroup->lastAdvanceMs = nowMs;
				}
				float t = clk.time;
				if (looped) {
					t = std::fmod(t, duration);
					if (t < 0.0f) {
						t += duration;
					}
				} else {
					t = std::clamp(t, 0.0f, duration);
				}
				localTime = t;
			} else {
				if (soloClock.ShouldAdvance(a_token)) {
					soloClock.time += a_deltaTime * speed;
					if (looped) {
						soloClock.time = std::fmod(soloClock.time, duration);
						if (soloClock.time < 0.0f) {
							soloClock.time += duration;
						}
					} else {
						soloClock.time = std::clamp(soloClock.time, 0.0f, duration);
					}
				}
				localTime = soloClock.time;
			}
		}

		// The ozz sample itself is deferred to StampPose. AnimationManager::Update
		// fires ~7x per render frame (subdivided dt), but only the pose composed
		// by BGSModelNode::Update is ever rendered — sampling here would compute
		// and discard ~6 of every 7 full-skeleton evaluations. Sample only
		// advances time/stage/cues and keeps the rig binding warm (above);
		// StampPose samples once per frame at the time accumulated here.
	}

	void Graph::StampPose(const RE::BGSModelNode* a_modelNode)
	{
		if (a_modelNode != cachedModelNode || binding.empty() || !anim || !skeleton) {
			return;
		}

		// Blend weight toward our pose. The slot content at this point is the
		// engine's live pose for this frame (its applier refreshed it after the
		// last graph update), so blending against the slot blends against
		// whatever the actor would otherwise be doing.
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

		// Sample the active clip at the time Sample accumulated on the update
		// stream. This is the single pose the engine is about to compose+commit,
		// so it runs once per render frame per skeleton (not once per subdivided
		// AnimationManager::Update slice). Serialized with Sample by `lock` (the
		// stamp hook holds it unique), so the sampling context/buffers stay
		// single-writer.
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
				REX::ERROR("Graph: ozz SamplingJob failed validation (anim tracks={}, context max={}, output soa={})",
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

		if (weight >= 1.0f) {
			for (const auto& [rigIdx, jointIdx] : binding) {
				WriteNiTransformRows(buf + static_cast<size_t>(rigIdx) * 16, outputPose[jointIdx]);
			}
		} else {
			const bool fromSnapshot = blendPhase == BlendPhase::kIn && blendFromValid;
			for (const auto& [rigIdx, jointIdx] : binding) {
				float* slot = buf + static_cast<size_t>(rigIdx) * 16;
				const float* from = fromSnapshot ?
				                        reinterpret_cast<const float*>(&blendFromPose[jointIdx]) :
				                        slot;  // engine's live pose this frame
				WriteNiTransformRowsBlended(slot, from, outputPose[jointIdx], weight);
			}
		}

		if (!loggedFirstApply) {
			loggedFirstApply = true;
			REX::INFO("Graph: first pose stamped pre-compose ({} bones)", binding.size());
		}
	}
}
