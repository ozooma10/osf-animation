#pragma once

// Pure local-pose transform helpers shared by the runtime stamper and the engine-free
// additive-pose tests. Matrix memory follows the existing Graph convention:
// column-major to the math here, byte-identical to Starfield's row-vector NiMatrix3 rows.

#include <algorithm>
#include <cmath>
#include <cstring>

namespace OSF::Animation::PoseMath
{
	struct Quat
	{
		float w = 1.0f;
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	inline Quat Normalize(Quat a_q)
	{
		const float lenSq = a_q.w * a_q.w + a_q.x * a_q.x + a_q.y * a_q.y + a_q.z * a_q.z;
		if (lenSq <= 1e-12f || !std::isfinite(lenSq)) {
			return {};
		}
		const float inv = 1.0f / std::sqrt(lenSq);
		return { a_q.w * inv, a_q.x * inv, a_q.y * inv, a_q.z * inv };
	}

	// Hamilton product. With the column-matrix interpretation used by Ozz,
	// Multiply(a, b) produces R(a) * R(b): b acts first, then a.
	inline Quat Multiply(const Quat& a_lhs, const Quat& a_rhs)
	{
		return Normalize({
			a_lhs.w * a_rhs.w - a_lhs.x * a_rhs.x - a_lhs.y * a_rhs.y - a_lhs.z * a_rhs.z,
			a_lhs.w * a_rhs.x + a_lhs.x * a_rhs.w + a_lhs.y * a_rhs.z - a_lhs.z * a_rhs.y,
			a_lhs.w * a_rhs.y - a_lhs.x * a_rhs.z + a_lhs.y * a_rhs.w + a_lhs.z * a_rhs.x,
			a_lhs.w * a_rhs.z + a_lhs.x * a_rhs.y - a_lhs.y * a_rhs.x + a_lhs.z * a_rhs.w
		});
	}

	inline Quat InverseUnit(const Quat& a_q)
	{
		const Quat q = Normalize(a_q);
		return { q.w, -q.x, -q.y, -q.z };
	}

	// a_m = 16 floats; 3x3 at [0..2]/[4..6]/[8..10], M(r,c) = a_m[c*4 + r].
	// This is the original override-path conversion and intentionally does not alter the basis.
	inline Quat MatrixToQuat(const float* a_m)
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

	// Normalize the three basis columns first so authored sample scale cannot leak into
	// additive rotation. Starfield's separate NiTransform scale remains engine-driven.
	inline Quat MatrixToUnitQuat(const float* a_m)
	{
		float unit[12]{
			a_m[0], a_m[1], a_m[2], 0.0f,
			a_m[4], a_m[5], a_m[6], 0.0f,
			a_m[8], a_m[9], a_m[10], 0.0f
		};
		for (int c = 0; c < 3; ++c) {
			float* axis = unit + c * 4;
			const float lenSq = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
			if (lenSq > 1e-12f && std::isfinite(lenSq)) {
				const float inv = 1.0f / std::sqrt(lenSq);
				axis[0] *= inv;
				axis[1] *= inv;
				axis[2] *= inv;
			}
		}
		return Normalize(MatrixToQuat(unit));
	}

	inline float EffectiveWeight(float a_transitionWeight, float a_poseWeight)
	{
		const float transition = std::isfinite(a_transitionWeight) ? std::clamp(a_transitionWeight, 0.0f, 1.0f) : 0.0f;
		const float pose = std::isfinite(a_poseWeight) ? std::clamp(a_poseWeight, 0.0f, 1.0f) : 1.0f;
		return transition * pose;
	}

	inline void QuatToMatrix3x3(const Quat& a_input, float* a_m)
	{
		const Quat& q = a_input;
		const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
		const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
		const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
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

	inline Quat Nlerp(const Quat& a_from, Quat a_to, float a_t)
	{
		const float dot = a_from.w * a_to.w + a_from.x * a_to.x +
			a_from.y * a_to.y + a_from.z * a_to.z;
		if (dot < 0.0f) {
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
			q = {};
		}
		return q;
	}

	inline Quat Slerp(const Quat& a_fromInput, Quat a_to, float a_t)
	{
		const Quat from = Normalize(a_fromInput);
		a_to = Normalize(a_to);
		float dot = from.w * a_to.w + from.x * a_to.x + from.y * a_to.y + from.z * a_to.z;
		if (dot < 0.0f) {
			a_to = { -a_to.w, -a_to.x, -a_to.y, -a_to.z };
			dot = -dot;
		}
		dot = std::clamp(dot, -1.0f, 1.0f);
		const float t = std::clamp(a_t, 0.0f, 1.0f);
		if (dot > 0.9995f) {
			return Nlerp(from, a_to, t);
		}
		const float angle = std::acos(dot);
		const float sinAngle = std::sin(angle);
		if (std::abs(sinAngle) <= 1e-8f) {
			return from;
		}
		const float fromScale = std::sin((1.0f - t) * angle) / sinAngle;
		const float toScale = std::sin(t * angle) / sinAngle;
		return Normalize({
			from.w * fromScale + a_to.w * toScale,
			from.x * fromScale + a_to.x * toScale,
			from.y * fromScale + a_to.y * toScale,
			from.z * fromScale + a_to.z * toScale
		});
	}

	// Per-bone decision for the override stamp path (StampPose), extracted so the zero-weight
	// boundary behavior is testable without an engine link.
	enum class OverrideStampKind : std::uint8_t
	{
		kSkip,      // leave the engine pose alone
		kSnapshot,  // stamp the cross-fade-from snapshot verbatim (zero-weight stage handoff)
		kTarget,    // stamp the sampled pose absolutely (total weight >= 1)
		kBlend      // cross-fade from -> target at the total weight
	};

	// a_weight = transition blend weight (exactly 0 on the first stamp after a stage/node handoff
	// resets the blend clock), a_poseWeight = persistent layer strength, a_maskWeight = the bone's
	// mask weight, a_fromSnapshot = the outgoing stage stamped this joint and its snapshot is valid.
	// A zero TOTAL weight normally leaves the engine pose in place — EXCEPT at the handoff boundary
	// with a live snapshot, where skipping would expose the vanilla pose for the compose(s) before
	// the next animation update advances the clock: clip A -> B must begin at A, not flash through
	// vanilla. A deliberately zero-strength role or bone (poseWeight/maskWeight zero) still skips.
	inline OverrideStampKind ClassifyOverrideStamp(float a_weight, float a_poseWeight,
		float a_maskWeight, bool a_fromSnapshot)
	{
		const float w = a_weight * a_poseWeight * a_maskWeight;
		if (w <= 0.0f) {
			return a_fromSnapshot && a_weight <= 0.0f && a_poseWeight > 0.0f && a_maskWeight > 0.0f ?
			           OverrideStampKind::kSnapshot :
			           OverrideStampKind::kSkip;
		}
		return w >= 1.0f ? OverrideStampKind::kTarget : OverrideStampKind::kBlend;
	}

	// Existing override cross-fade behavior, extracted unchanged for focused regression tests.
	inline void WriteOverrideBlended(float* a_slot, const float* a_from, const float* a_target, float a_weight)
	{
		const Quat q = Nlerp(MatrixToQuat(a_from), MatrixToQuat(a_target), a_weight);
		const float tx = a_from[12] + (a_target[12] - a_from[12]) * a_weight;
		const float ty = a_from[13] + (a_target[13] - a_from[13]) * a_weight;
		const float tz = a_from[14] + (a_target[14] - a_from[14]) * a_weight;
		QuatToMatrix3x3(q, a_slot);
		a_slot[3] = a_slot[7] = a_slot[11] = 0.0f;
		a_slot[12] = tx;
		a_slot[13] = ty;
		a_slot[14] = tz;
		a_slot[15] = 1.0f;
	}

	// Compose the sampled local-to-parent delta over an immutable live local-to-parent base:
	//   delta = inverse(reference) * sampled
	//   final = live * slerp(identity, delta, weight)
	// The product order matches Ozz's column-matrix convention. Starfield consumes the same bytes
	// as row-vector NiMatrix3 rows, where this appears transposed with the equivalent reversed row order.
	inline void WriteAdditive(float* a_slot, const float* a_live, const float* a_reference,
		const float* a_sampled, float a_weight)
	{
		const float weight = std::clamp(a_weight, 0.0f, 1.0f);
		const float dx = a_sampled[12] - a_reference[12];
		const float dy = a_sampled[13] - a_reference[13];
		const float dz = a_sampled[14] - a_reference[14];
		const Quat reference = MatrixToUnitQuat(a_reference);
		const Quat sampled = MatrixToUnitQuat(a_sampled);
		const Quat delta = Multiply(InverseUnit(reference), sampled);

		// Exact fast paths are also what make weight zero and an exact rest-pose sample byte-for-byte
		// no-ops on the live engine transform.
		const float deltaVectorSq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
		if (weight <= 0.0f ||
			(deltaVectorSq <= 1e-12f && std::abs(delta.w) >= 0.999999f &&
				dx == 0.0f && dy == 0.0f && dz == 0.0f)) {
			std::memcpy(a_slot, a_live, 16 * sizeof(float));
			return;
		}

		const Quat weightedDelta = Slerp({}, delta, weight);
		const Quat finalRotation = Multiply(MatrixToUnitQuat(a_live), weightedDelta);
		const float pad0 = a_live[3], pad1 = a_live[7], pad2 = a_live[11];
		const float liveScale = a_live[15];
		QuatToMatrix3x3(finalRotation, a_slot);
		a_slot[3] = pad0;
		a_slot[7] = pad1;
		a_slot[11] = pad2;
		a_slot[12] = a_live[12] + dx * weight;
		a_slot[13] = a_live[13] + dy * weight;
		a_slot[14] = a_live[14] + dz * weight;
		a_slot[15] = liveScale;
	}
}
