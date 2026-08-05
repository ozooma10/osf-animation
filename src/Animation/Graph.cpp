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

		using Matrix = ozz::math::Float4x4;
		const float* M(const Matrix& a_m) { return reinterpret_cast<const float*>(&a_m); }
		float* M(Matrix& a_m) { return reinterpret_cast<float*>(&a_m); }
		Matrix Mul(const Matrix& a, const Matrix& b)
		{
			Matrix out = Matrix::identity();
			for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) {
				M(out)[c * 4 + r] = 0.0f;
				for (int k = 0; k < 4; ++k) M(out)[c * 4 + r] += M(a)[k * 4 + r] * M(b)[c * 4 + k];
			}
			return out;
		}
		Matrix RigidInverse(const Matrix& a)
		{
			Matrix out = Matrix::identity();
			for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) M(out)[c * 4 + r] = M(a)[r * 4 + c];
			for (int r = 0; r < 3; ++r) M(out)[12 + r] = -(M(out)[r] * M(a)[12] + M(out)[4 + r] * M(a)[13] + M(out)[8 + r] * M(a)[14]);
			return out;
		}
		struct V3 { float x, y, z; };
		V3 Pos(const Matrix& a) { return { M(a)[12], M(a)[13], M(a)[14] }; }
		V3 Sub(V3 a, V3 b) { return { a.x-b.x, a.y-b.y, a.z-b.z }; }
		V3 Add(V3 a, V3 b) { return { a.x+b.x, a.y+b.y, a.z+b.z }; }
		V3 Scale(V3 a, float s) { return { a.x*s, a.y*s, a.z*s }; }
		float Dot(V3 a, V3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
		V3 Cross(V3 a, V3 b) { return { a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x }; }
		float Len(V3 a) { return std::sqrt(Dot(a,a)); }
		V3 Unit(V3 a) { const float n=Len(a); return n>1e-6f ? Scale(a,1.0f/n) : V3{1,0,0}; }
		PoseMath::Quat FromTo(V3 a, V3 b)
		{
			a=Unit(a); b=Unit(b); const float d=std::clamp(Dot(a,b),-1.0f,1.0f);
			if (d > .99999f) return {};
			if (d < -.99999f) { V3 axis=Unit(std::abs(a.x)<.8f ? Cross(a,{1,0,0}) : Cross(a,{0,1,0})); return {0,axis.x,axis.y,axis.z}; }
			const V3 c=Cross(a,b); return PoseMath::Normalize({1.0f+d,c.x,c.y,c.z});
		}
		Matrix Transform(PoseMath::Quat q, V3 p)
		{
			Matrix out=Matrix::identity(); PoseMath::QuatToMatrix3x3(q,M(out)); M(out)[12]=p.x; M(out)[13]=p.y; M(out)[14]=p.z; return out;
		}
		PoseMath::Quat EulerDegrees(const std::array<float,3>& e)
		{
			constexpr float k=.00872664626f; const float cx=std::cos(e[0]*k), sx=std::sin(e[0]*k), cy=std::cos(e[1]*k), sy=std::sin(e[1]*k), cz=std::cos(e[2]*k), sz=std::sin(e[2]*k);
			return PoseMath::Multiply(PoseMath::Multiply({cz,0,0,sz},{cy,0,sy,0}),{cx,sx,0,0});
		}
		float ReachWeight(const LiveReach& r, float t)
		{
			const float begin=r.atSeconds-r.approachSeconds, full=r.atSeconds-r.contactLeadSeconds, releaseBegin=r.atSeconds+r.contactTrailSeconds, end=releaseBegin+r.releaseSeconds;
			if (t<=begin || t>=end) return 0.0f; if (t<full) return (t-begin)/std::max(full-begin,1e-5f); if (t<=releaseBegin) return 1.0f; return 1.0f-(t-releaseBegin)/std::max(end-releaseBegin,1e-5f);
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
			blendFromDriven.assign(outputPose.size(), 0);
			for (const auto& bound : binding) {
				if (bound.jointIndex < blendFromDriven.size()) {
					blendFromDriven[bound.jointIndex] = 1;
				}
			}
			blendFromValid = true;
		} else {
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
		hasPose = false;
		holdClipAtEnd = false;
		if(!scene) {
			syncGroup = std::make_shared<SyncGroup>();	// new clip owns the clock if not in a scene
		}

		const int numJoints = skeleton->data->num_joints();
		samplingContext.Resize(numJoints);
		localPose.resize(skeleton->data->num_soa_joints());
		outputPose.assign(numJoints, ozz::math::Float4x4::identity());
		referencePose.assign(numJoints, ozz::math::Float4x4::identity());
		reachLocalPose.assign(numJoints, ozz::math::Float4x4::identity());
		reachModelPose.assign(numJoints, ozz::math::Float4x4::identity());
		reachSampledModelPose.assign(numJoints, ozz::math::Float4x4::identity());
		reachDriven.assign(numJoints, 0);
		rigIndexByJoint.assign(numJoints, UINT16_MAX);
		const auto restPose = skeleton->data->joint_rest_poses();
		UnpackSoaTransforms({ restPose.data(), restPose.size() },
			{ referencePose.data(), referencePose.size() }, skeleton->data.get());

		jointMap.clear();
		auto jointNames = skeleton->data->joint_names();
		for (size_t i = 0; i < jointNames.size(); i++) {
			jointMap[ToLower(jointNames[i])] = static_cast<uint16_t>(i);
		}

		// force a re-bind against the (possibly unchanged) rig
		InvalidateBinding();
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

		// The binding is the write policy. Force it to rebuild even if the live rig itself did not change.
		InvalidateBinding();
	}

	void Graph::SetBoneMask(const std::string& a_maskName)
	{
		const BoneMask::Mask* next = a_maskName.empty() ? nullptr : BoneMask::Find(a_maskName);
		if (!a_maskName.empty() && !next) {
			// The registry validates mask names at load; reaching this means a caller bypassed it.
			REX::WARN("[Anim] unknown bone mask '{}' — playing unmasked (known: {})",
				a_maskName, BoneMask::KnownList());
		}
		if (next == boneMask) {
			return;  // unchanged policy keeps the live binding
		}
		boneMask = next;
		loggedMaskBind = false;
		// The mask decides which bones bind. Force a rebuild even if the live rig did not change.
		InvalidateBinding();
	}

	void Graph::SetLiveReach(const LiveReach& a_reach)
	{
		liveReach = a_reach;
		reachTargetFrozen = false;
		loggedReachDisabled = false;
		loggedReachContact = false;
	}

	void Graph::SetContactPose(const ContactPose& a_contactPose)
	{
		contactPose = a_contactPose;
		contactPoseBones.clear();
		for (const auto& bone : contactPose.bones) {
			if (!bone.empty()) contactPoseBones.emplace(ToLower(bone));
		}
		loggedContactPoseFull = false;
		// Contact bones extend the ordinary stage mask, so the write binding must be rebuilt.
		InvalidateBinding(true);
	}

	void Graph::ApplyLiveReach(float* a_liveBuffer, std::uint16_t a_rigBoneCount)
	{
		if (!liveReach.enabled || !skeleton || reachLocalPose.size() != outputPose.size()) return;
		const float reachWeight = ReachWeight(liveReach, localTime);
		if (reachWeight <= 0.0f) return;
		const auto joint = [&](std::string_view name) -> int { const auto it=jointMap.find(ToLower(name)); return it==jointMap.end() ? -1 : static_cast<int>(it->second); };
		const int targetJoint=joint(liveReach.targetBone), carrierJoint=joint(liveReach.carrierBone);
		const auto limbJoints = [&](std::string_view limb) -> std::array<int,3> {
			const bool right=limb=="rightArm"; return {joint(right?"R_Biceps":"L_Biceps"),joint(right?"R_Forearm":"L_Forearm"),joint(right?"R_Wrist":"L_Wrist")};
		};
		const auto primary=limbJoints(liveReach.primaryLimb);
		if (targetJoint<0 || carrierJoint<0 || primary[0]<0 || primary[1]<0 || primary[2]<0 ||
			targetJoint>=static_cast<int>(rigIndexByJoint.size()) || rigIndexByJoint[targetJoint]==UINT16_MAX) {
			if (!loggedReachDisabled) { loggedReachDisabled=true; REX::WARN("[Anim] live reach disabled for actor {:08X}: required target/carrier/limb bones are unavailable", target ? target->formID : 0); }
			return;
		}
		std::fill(reachDriven.begin(),reachDriven.end(),0);
		for (const auto& b:binding) if (b.jointIndex<reachDriven.size()) reachDriven[b.jointIndex]=1;
		if (!reachDriven[carrierJoint] || !reachDriven[primary[0]] || !reachDriven[primary[1]] || !reachDriven[primary[2]]) {
			if (!loggedReachDisabled) { loggedReachDisabled=true; REX::WARN("[Anim] live reach disabled for actor {:08X}: active mask does not drive the primary limb and carrier", target ? target->formID : 0); }
			return;
		}
		for (std::size_t i=0;i<outputPose.size();++i) {
			reachLocalPose[i]=outputPose[i];
			const auto ri=rigIndexByJoint[i];
			if (!reachDriven[i] && ri!=UINT16_MAX && ri<a_rigBoneCount) std::memcpy(M(reachLocalPose[i]),a_liveBuffer+static_cast<std::size_t>(ri)*16,16*sizeof(float));
		}
		const auto parents=skeleton->data->joint_parents();
		const auto rebuild=[&]() { for (std::size_t i=0;i<reachLocalPose.size();++i) reachModelPose[i]=parents[i]<0 ? reachLocalPose[i] : Mul(reachModelPose[parents[i]],reachLocalPose[i]); };
		for (std::size_t i=0;i<outputPose.size();++i) {
			reachSampledModelPose[i]=parents[i]<0 ? outputPose[i] : Mul(reachSampledModelPose[parents[i]],outputPose[i]);
		}
		rebuild();
		if (parents[carrierJoint] != primary[2]) {
			if (!loggedReachDisabled) { loggedReachDisabled=true; REX::WARN("[Anim] live reach disabled for actor {:08X}: carrier is not a direct child of the primary wrist", this->target ? this->target->formID : 0); }
			return;
		}
		const Matrix targetSocket=Transform(EulerDegrees(liveReach.targetRotationDegrees),{liveReach.targetTranslation[0],liveReach.targetTranslation[1],liveReach.targetTranslation[2]});
		Matrix reachTarget=Mul(reachModelPose[targetJoint],targetSocket);
		const Matrix authoredTarget=Mul(reachSampledModelPose[targetJoint],targetSocket);
		if (liveReach.tracking==ReachTracking::kFreezeAtContact && localTime>=liveReach.atSeconds) {
			if (!reachTargetFrozen) { std::memcpy(frozenReachTarget.data(),M(reachTarget),16*sizeof(float)); reachTargetFrozen=true; }
			std::memcpy(M(reachTarget),frozenReachTarget.data(),16*sizeof(float));
		}
		const Matrix authoredCarrier=reachSampledModelPose[carrierJoint];
		const Matrix currentCarrier=reachModelPose[carrierJoint];
		const Matrix retargetedCarrier=Mul(Mul(reachTarget,RigidInverse(authoredTarget)),authoredCarrier);
		V3 delta=Sub(Pos(retargetedCarrier),Pos(currentCarrier)); const float dn=Len(delta); if (dn>liveReach.maxCorrection) delta=Scale(delta,liveReach.maxCorrection/dn);
		const auto authoredQ=PoseMath::MatrixToUnitQuat(M(currentCarrier)); const auto targetQ=PoseMath::MatrixToUnitQuat(M(retargetedCarrier));
		float angle=2.0f*std::acos(std::clamp(std::abs(authoredQ.w*targetQ.w+authoredQ.x*targetQ.x+authoredQ.y*targetQ.y+authoredQ.z*targetQ.z),0.0f,1.0f));
		if (!std::isfinite(dn) || !std::isfinite(angle) || dn > liveReach.maxCorrection * 2.0f || angle > liveReach.maxCorrectionRadians * 1.5f) {
			if (!loggedReachDisabled) {
				loggedReachDisabled=true;
				REX::WARN("[Anim] live reach skipped for actor {:08X}: retarget delta {:.1f} cm / {:.1f} deg exceeds safe envelope",
					target ? target->formID : 0, dn * 100.0f, angle * 57.2957795f);
			}
			return;
		}
		if (!loggedReachContact && reachWeight >= 0.99f) {
			loggedReachContact = true;
			REX::DEBUG("[Anim] live reach contact — actor {:08X}, target '{}', carrier '{}', correction {:.1f} cm / {:.1f} deg{}",
				target ? target->formID : 0, liveReach.targetBone, liveReach.carrierBone,
				dn * 100.0f, angle * 57.2957795f, dn > liveReach.maxCorrection || angle > liveReach.maxCorrectionRadians ? " (clamped)" : "");
		}
		const float rotationT=angle>1e-5f ? std::min(1.0f,liveReach.maxCorrectionRadians/angle)*reachWeight : reachWeight;
		Matrix desiredCarrier=Transform(PoseMath::Slerp(authoredQ,targetQ,rotationT),Add(Pos(currentCarrier),Scale(delta,reachWeight)));
		Matrix secondaryRelation=Matrix::identity(); std::array<int,3> secondary{-1,-1,-1};
		if (!liveReach.secondaryLimbs.empty()) {
			secondary=limbJoints(liveReach.secondaryLimbs.front());
			if (secondary[0]<0 || secondary[1]<0 || secondary[2]<0 || !reachDriven[secondary[0]] || !reachDriven[secondary[1]] || !reachDriven[secondary[2]]) {
				if (!loggedReachDisabled) { loggedReachDisabled=true; REX::WARN("[Anim] live reach secondary limb unavailable for actor {:08X}; continuing with primary hand only", target ? target->formID : 0); }
				secondary={-1,-1,-1};
			} else secondaryRelation=Mul(RigidInverse(authoredCarrier),reachSampledModelPose[secondary[2]]);
		}
		const Matrix desiredWrist=Mul(desiredCarrier,RigidInverse(reachLocalPose[carrierJoint]));
		const auto solve=[&](const std::array<int,3>& chain,const Matrix& goal) {
			if (chain[0]<0||chain[1]<0||chain[2]<0) return;
			rebuild();
			const V3 root=Pos(reachModelPose[chain[0]]), mid=Pos(reachModelPose[chain[1]]), end=Pos(reachModelPose[chain[2]]), targetPosition=Pos(goal);
			const float upperLength=Len(Sub(mid,root)), lowerLength=Len(Sub(end,mid));
			if (upperLength<1e-6f || lowerLength<1e-6f) return;
			V3 toTarget=Sub(targetPosition,root); const float distance=std::max(Len(toTarget),1e-5f); const V3 direction=Unit(toTarget);
			const float cosAngle=std::clamp((upperLength*upperLength+distance*distance-lowerLength*lowerLength)/(2.0f*upperLength*distance),-1.0f,1.0f);
			const float sinAngle=std::sqrt(std::max(0.0f,1.0f-cosAngle*cosAngle));
			V3 bend=Sub(Sub(mid,root),Scale(direction,Dot(Sub(mid,root),direction)));
			if (Len(bend)<1e-6f) bend=std::abs(direction.y)<0.9f ? Cross(direction,{0,1,0}) : Cross(direction,{1,0,0});
			bend=Unit(bend);
			const V3 solvedMid=Add(Add(root,Scale(direction,upperLength*cosAngle)),Scale(bend,upperLength*sinAngle));
			const auto rotateJoint=[&](int j,V3 from,V3 to) {
				const auto d=FromTo(from,to), worldQ=PoseMath::MatrixToUnitQuat(M(reachModelPose[j]));
				const auto parentQ=parents[j]<0 ? PoseMath::Quat{} : PoseMath::MatrixToUnitQuat(M(reachModelPose[parents[j]]));
				PoseMath::QuatToMatrix3x3(PoseMath::Multiply(PoseMath::InverseUnit(parentQ),PoseMath::Multiply(d,worldQ)),M(reachLocalPose[j]));
			};
			rotateJoint(chain[0],Sub(mid,root),Sub(solvedMid,root));
			rebuild();
			const V3 newMid=Pos(reachModelPose[chain[1]]), newEnd=Pos(reachModelPose[chain[2]]);
			rotateJoint(chain[1],Sub(newEnd,newMid),Sub(targetPosition,newMid));
			rebuild(); const auto parentQ=parents[chain[2]]<0 ? PoseMath::Quat{} : PoseMath::MatrixToUnitQuat(M(reachModelPose[parents[chain[2]]]));
			PoseMath::QuatToMatrix3x3(PoseMath::Multiply(PoseMath::InverseUnit(parentQ),PoseMath::MatrixToUnitQuat(M(goal))),M(reachLocalPose[chain[2]])); rebuild();
		};
		solve(primary,desiredWrist);
		// A small carrier-local residual closes normal solver error without allowing visible hand separation.
		rebuild(); const Matrix desiredLocal=Mul(RigidInverse(reachModelPose[parents[carrierJoint]]),desiredCarrier); const V3 currentLocal=Pos(reachLocalPose[carrierJoint]);
		V3 residual=Sub(Pos(desiredLocal),currentLocal); const float rn=Len(residual); if (rn>liveReach.maxResidual) residual=Scale(residual,liveReach.maxResidual/rn);
		M(reachLocalPose[carrierJoint])[12]+=residual.x; M(reachLocalPose[carrierJoint])[13]+=residual.y; M(reachLocalPose[carrierJoint])[14]+=residual.z;
		const auto cq=PoseMath::MatrixToUnitQuat(M(reachLocalPose[carrierJoint])), dq=PoseMath::MatrixToUnitQuat(M(desiredLocal));
		const float ra=2.0f*std::acos(std::clamp(std::abs(cq.w*dq.w+cq.x*dq.x+cq.y*dq.y+cq.z*dq.z),0.0f,1.0f));
		PoseMath::QuatToMatrix3x3(PoseMath::Slerp(cq,dq,ra>1e-5f?std::min(1.0f,liveReach.maxResidualRadians/ra):1.0f),M(reachLocalPose[carrierJoint])); rebuild();
		if (secondary[2]>=0) solve(secondary,Mul(reachModelPose[carrierJoint],secondaryRelation));
		for (int j:primary) outputPose[j]=reachLocalPose[j]; if (secondary[0]>=0) for(int j:secondary) outputPose[j]=reachLocalPose[j]; outputPose[carrierJoint]=reachLocalPose[carrierJoint];
	}

	bool Graph::ResolveAndBind(const RE::BGSModelNode* a_expectedModelNode)
	{

		//failure invalidates the binding cache.
		//cachedModelNode is the stamp hooks match key. Once chain stops resolving (3d detatched), the cached address can be freed and reused.
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
			// A failure while the COMPOSE hook expected a specific node means composes are running
			// unstamped (a visible vanilla-pose frame) — the previously silent glitch path.
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
			// An unrelated skeleton reached the game-wide compose hook while this graph was
			// temporarily unbound. Keep waiting for the managed actor's current node.
			return false;
		}

		auto* rig = modelNode->rig;
		if (!rig || !rig->local || !rig->local->data) {
			return fail();
		}

		const auto* previousModelNode = cachedModelNode.load(std::memory_order_relaxed);
		// Cache identity includes the buffer base (rig->local->data) and rigBoneCount so a modelNode
		// that was freed and reused at the same address with a fresh/smaller rig buffer forces a rebind
		// instead of reusing stale rigIndices that now point past the live buffer.
		if (modelNode == previousModelNode && rig == cachedRig
			&& rig->local->data == cachedLocalData
			&& modelNode->nodes.size() == cachedBoneCount
			&& GetRigBoneCount(modelNode) == cachedRigBoneCount) {
			return !binding.empty();
		}

		// DIAG: a rebind to a DIFFERENT non-null modelNode after the first bind means the actor's 3D was
		// rebuilt under us (e.g. equipment restore at scene end). Tells us whether a FADING graph re-binds to
		// the freshly-built skeleton and could keep stamping it. Once per swap.
		if (previousModelNode && previousModelNode != modelNode) {
			REX::TRACE("[Anim] rig REBIND — modelNode {} -> {} (3d rebuilt; blendPhase {})",
				static_cast<const void*>(previousModelNode), static_cast<const void*>(modelNode),
				static_cast<int>(blendPhase));
		}
		// DIAG: same node, changed identity = the node table / rig buffer was mutated in place
		// (helmet visibility patches, prop geometry). These mid-play rebinds were invisible
		// before (the bind log is once per graph) yet they bracket the glitch-frame windows.
		if (previousModelNode == modelNode && loggedBind) {
			REX::TRACE("[Anim] rig re-bind — same modelNode, identity changed (nodes {} -> {}, rigBones {} -> {})",
				cachedBoneCount, modelNode->nodes.size(), cachedRigBoneCount, GetRigBoneCount(modelNode));
		}

		// build the rigIndex -> jointIndex binding from the bone map
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
				float contactWeight = 0.0f;
				if (boneMask) {
					const auto mit = boneMask->weights.find(lowerName);
					if (mit == boneMask->weights.end()) {
						if (contactPose.enabled && contactPoseBones.contains(lowerName)) {
							boneWeight = 0.0f;
							contactWeight = 1.0f;
						} else {
							skippedMasked++;  // outside the driven whitelist — stays engine-driven
							continue;
						}
					} else {
						boneWeight = mit->second;
					}
				}
				binding.push_back({ entry.rigIndex, iter->second, boneWeight, contactWeight });
			}
		}
		if (++bindingRevision == 0) {
			++bindingRevision;  // zero is reserved for a binding that has never existed
		}
		// Allocate the immutable live-base buffer only when the binding changes, never in StampPose.
		// A compose-time rebind may run before Starfield evaluates the new rig. Seed it from the last
		// proven base by skeleton joint, not the remapped rig-slot order, until Sample authorizes capture.
		liveBasePose.assign(binding.size() * 16, 0.0f);
		liveBaseValid = liveBaseCache.Restore(
			std::span{ binding }, std::span{ liveBasePose });
		basePoseRevision = 0;
		evaluatedBinding.Clear();
		stampProbeValid = false;  // rig indices may have been remapped; the old probe is meaningless

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
			// 0 driven = the mask matched nothing on this rig (off-species) — the graph stamps nothing.
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
		// Drop the model-node cache: if the engine stops sampling this graph mid-fade (actor
		// unloaded), a stale cachedModelNode could later match a REUSED address and stamp a
		// foreign actor's rig. Sample re-resolves and re-binds on the next update, so a live
		// fade keeps stamping.
		InvalidateBinding(true);
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
				SetPosePolicy(stage.poseModes[participantIndex], stage.poseWeights[participantIndex], roleName);
				SetBoneMask(stage.masks[participantIndex]);
				SetLiveReach(stage.liveReach[participantIndex]);
				SetContactPose(stage.contactPose[participantIndex]);
				blendDuration = stage.blendIn;  // per-stage blend-in
				scenePlacement = stage.placements[participantIndex];
				appliedStage = tick.stage;
			}
			localTime = tick.time;
			// A finished one-shot stage holds its final frame (never wraps); the flag sticks
			// through DetachAndFadeOut so the fade-out below also samples the end pose.
			holdClipAtEnd = tick.holdEnd;
			// The scene's stage boundary uses its reference clip, but malformed packs can contain
			// slightly different participant durations. Keep every SamplingJob ratio in [0,1) by
			// wrapping against this graph's own clip instead of feeding ozz an invalid ratio.
			if (anim && anim->data && anim->data->duration() > 0.0f) {
				localTime = holdClipAtEnd ? std::min(localTime, anim->data->duration()) :
				                            std::fmod(localTime, anim->data->duration());
			}
		}

		if (!anim || !skeleton) {
			return;
		}

		if (!ResolveAndBind()) {
			return;
		}
		// Hook_AnimGraphUpdate calls Sample only after Starfield evaluated this actor. Capture is now
		// safe for this exact binding generation; an earlier compose-time rebind cannot inherit it.
		evaluatedBinding.Mark(enginePoseRevision, bindingRevision);

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
			if (holdClipAtEnd) {
				// Ended one-shot fading out: hold the final frame, never wrap back to frame 0.
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

		// ozz sample defered to StampPose. AnimationManager::Update fires 7x per frame, so sampling here is waste.
		// this only advance time/stage/cues and keeps rig binding warm. StampPose samples once per frame at time accumulated here
	}

	void Graph::StampPose(const RE::BGSModelNode* a_modelNode)
	{
		if (a_modelNode != cachedModelNode.load(std::memory_order_relaxed) ||
			binding.empty() || !anim || !skeleton ||
			outputPose.size() != referencePose.size()) {
			return;
		}

		// Same-address staleness: helmet-visibility patches and prop geometry mutate the node
		// table (and can remap rig slots) WITHOUT replacing the BGSModelNode, and Sample only
		// notices on its next update-stream call. Stamping the old binding would write remapped
		// slots (a scrambled frame) — re-bind now, on the first compose that sees the mutation.
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
		const float contactWeight = contactPose.WeightAt(localTime);
		if (contactPose.enabled && !loggedContactPoseFull && contactWeight >= 0.99f) {
			loggedContactPoseFull = true;
			REX::DEBUG("[Anim] contact pose full — actor {:08X}, {} bone(s), contact {:.3f}s",
				target ? target->formID : 0, contactPoseBones.size(), contactPose.atSeconds);
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
		ApplyLiveReach(buf, rigBoneCount);

		// AnimationManager::Update evaluates Starfield first and increments enginePoseRevision via
		// Sample. Capture those fresh local slots once. If BGSModelNode::Update repeats before a new
		// engine evaluation, keep this immutable base instead of reading our own prior write. Shared
		// by additive layering and any persistent sub-unity override weight (feathered mask seam /
		// poseWeight), which would otherwise converge onto the full pose by re-blending over itself.
		const auto captureLiveBase = [&]() -> bool {
			if (liveBasePose.size() != binding.size() * 16) {
				return false;  // binding/base must be prepared together; never allocate from the stamp hook
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
				// The revision counts update-stream calls, not proven buffer writes. If the probe
				// slot still holds OUR previous write, the engine has not re-applied its pose since
				// we stamped — adopting the buffer would poison the feather/additive base with our
				// own output (the spine pops as it re-blends over itself). Keep the previous base.
				// Gated on a base actually existing (revision 0 = liveBasePose holds no capture yet).
				if (basePoseRevision != 0 && stampProbeValid && stampProbeRigIdx < rigBoneCount &&
					std::memcmp(buf + static_cast<std::size_t>(stampProbeRigIdx) * 16,
						stampProbe.data(), 16 * sizeof(float)) == 0) {
					basePoseRevision = enginePoseRevision;
					captureGateCount++;
					if (captureGateCount == 1 || (captureGateCount & 63u) == 0) {
						REX::TRACE("[Anim] live-base capture gated — buffer unchanged since our stamp (actor {:08X}, #{})",
							target ? target->formID : 0, captureGateCount);
					}
					return true;
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

		// Record the bytes we leave in the first written slot; the next capture's probe.
		bool probeRecorded = false;
		const auto recordProbe = [&](const float* a_slot, uint16_t a_rigIdx) {
			std::memcpy(stampProbe.data(), a_slot, 16 * sizeof(float));
			stampProbeRigIdx = a_rigIdx;
			stampProbeValid = true;
			probeRecorded = true;
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
					effectiveWeight * std::max(bound.weight, bound.contactWeight * contactWeight));
				if (!probeRecorded) {
					recordProbe(slot, bound.rigIndex);
				}
			}
		} else {
			// Override: per-bone weight = transition * persistent layer strength * mask weight.
			// Weight-1 bones take the sampled pose absolutely; fractional bones blend against the
			// immutable engine base (steady state) or the cross-fade-from snapshot (blend-in), so a
			// masked gesture overrides the arm chain while the feathered seam keeps live torso sway.
			const bool persistentPartial = (boneMask && boneMask->feathered) || poseWeight < 1.0f || contactPose.enabled;
			if (persistentPartial && !captureLiveBase()) {
				return;
			}
			const bool hasSnapshot = blendPhase == BlendPhase::kIn && blendFromValid;
			for (std::size_t i = 0; i < binding.size(); ++i) {
				const auto& bound = binding[i];
				if (bound.rigIndex >= rigBoneCount) {
					continue;
				}
				float* slot = buf + static_cast<std::size_t>(bound.rigIndex) * 16;
				// A stage-local mask may expand at this boundary. Only joints the outgoing
				// stage actually stamped have a valid OSF snapshot; newly admitted joints
				// blend from Starfield's live pose instead of the unshown parts of its clip.
				const bool fromSnapshot = hasSnapshot && bound.jointIndex < blendFromDriven.size() &&
				                          blendFromDriven[bound.jointIndex] != 0;
				const float maskWeight = std::max(bound.weight, bound.contactWeight * contactWeight);
				// Zero-total-weight bones normally skip; the stage-handoff snapshot case must not
				// (clip A -> B begins at A instead of flashing through vanilla). The decision is
				// PoseMath::ClassifyOverrideStamp so the boundary behavior is pinned by tests.
				const float w = weight * poseWeight * maskWeight;
				switch (PoseMath::ClassifyOverrideStamp(weight, poseWeight, maskWeight, fromSnapshot)) {
				case PoseMath::OverrideStampKind::kSkip:
					continue;
				case PoseMath::OverrideStampKind::kSnapshot:
					WriteNiTransformRowsBlended(slot,
						reinterpret_cast<const float*>(&blendFromPose[bound.jointIndex]),
						outputPose[bound.jointIndex], 0.0f);
					break;
				case PoseMath::OverrideStampKind::kTarget:
					WriteNiTransformRows(slot, outputPose[bound.jointIndex]);
					break;
				case PoseMath::OverrideStampKind::kBlend:
					{
						const float* from = fromSnapshot ?
						                        reinterpret_cast<const float*>(&blendFromPose[bound.jointIndex]) :
						                        (persistentPartial ? liveBasePose.data() + i * 16 :
						                                             slot);  // engine's live pose this frame
						WriteNiTransformRowsBlended(slot, from, outputPose[bound.jointIndex], w);
					}
					break;
				}
				if (!probeRecorded) {
					recordProbe(slot, bound.rigIndex);
				}
			}
		}
		if (!probeRecorded) {
			stampProbeValid = false;  // nothing written this stamp (all bones skipped) — no probe
		}

		if (!loggedFirstApply) {
			loggedFirstApply = true;
			REX::TRACE("[Anim] first pose stamped pre-compose ({} bones)", binding.size());
		}
	}
}
