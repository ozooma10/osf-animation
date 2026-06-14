#pragma once

// Ozz wrapper types and SoA/AoS conversion helpers.
// Ported in reduced form from NativeAnimationFrameworkSF (Animation/Ozz.h, Util/Ozz.h)
// Copyright (C) Deweh, GPL-3.0 — https://github.com/Deweh/NativeAnimationFrameworkSF

#include <span>

#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/soa_float4x4.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/memory/unique_ptr.h"

namespace OSF::Animation
{
	struct OzzSkeleton
	{
		ozz::unique_ptr<ozz::animation::Skeleton> data;
	};

	struct OzzAnimation
	{
		ozz::unique_ptr<ozz::animation::Animation> data;
	};

	// Reduced LocalToModelJob: converts SoaTransforms to local-space Float4x4s
	// without any hierarchy math.
	inline void UnpackSoaTransforms(const std::span<const ozz::math::SoaTransform>& a_input,
		const std::span<ozz::math::Float4x4>& a_output,
		const ozz::animation::Skeleton* a_skeleton)
	{
		const int end = a_skeleton->num_joints();
		if (a_input.size() != static_cast<size_t>(a_skeleton->num_soa_joints()) ||
			a_output.size() != static_cast<size_t>(end)) {
			return;
		}

		for (int i = 0, process = i < end; process;) {
			const ozz::math::SoaTransform& transform = a_input[i / 4];
			const ozz::math::SoaFloat4x4 local_soa_matrices = ozz::math::SoaFloat4x4::FromAffine(
				transform.translation, transform.rotation, transform.scale);

			ozz::math::Float4x4 local_aos_matrices[4];
			ozz::math::Transpose16x16(&local_soa_matrices.cols[0].x,
				local_aos_matrices->cols);

			for (const int soa_end = (i + 4) & ~3; i < soa_end && process; ++i, process = i < end) {
				a_output[i] = local_aos_matrices[i & 3];
			}
		}
	}
}
