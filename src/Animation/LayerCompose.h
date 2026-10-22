#pragma once

// Engine-free, allocation-free composition into caller-owned storage. The caller supplies the
// ordered stack and omits role-preserved bones from each layer's binding.

#include "Animation/PoseMath.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace OSF::Animation::LayerCompose
{
	enum class Mode : std::uint8_t { kOverride, kAdditive };

	struct Bone
	{
		std::uint16_t jointIndex;
		float maskWeight;
	};

	struct Layer
	{
		Mode mode = Mode::kOverride;
		float poseWeight = 1.0f;
		float transition = 1.0f;
		const float* sampled = nullptr;    // 16 floats per joint
		const float* reference = nullptr;  // additive only
		const float* displayed = nullptr;  // outgoing snapshot, may be null
		const std::uint8_t* displayedDriven = nullptr;
		const Bone* bones = nullptr;
		std::size_t boneCount = 0;
	};

	// a_out is pre-seeded with base(0); a_drivenOut is cleared by the caller each frame.
	// Pose and driven arrays are indexed by joint and cover a_jointCount entries. Bone indices
	// are unique within a layer. Missing samples or an unproven additive base inherit a_out.
	// Returns false without touching either output when the stack exceeds the supported depth (1).
	inline bool Compose(const Layer* a_layers, std::size_t a_layerCount, bool a_haveBase,
		float* a_out, std::uint8_t* a_drivenOut, std::size_t a_jointCount)
	{
		if (a_layerCount > 1) {
			return false;  // Layer stack exceeds the supported depth of one.
		}
		if (a_layerCount == 0) {
			return true;
		}

		const Layer& layer = a_layers[0];
		if (!layer.sampled || !layer.bones ||
			(layer.mode == Mode::kAdditive && (!a_haveBase || !layer.reference))) {
			return true;
		}
		const float effectiveWeight = PoseMath::EffectiveWeight(layer.transition, layer.poseWeight);
		for (std::size_t i = 0; i < layer.boneCount; ++i) {
			const Bone& bone = layer.bones[i];
			const std::size_t joint = bone.jointIndex;
			if (joint >= a_jointCount) {
				continue;
			}
			const std::size_t offset = joint * 16;
			float* out = a_out + offset;
			// Keep the base immutable while the helper writes, including its memcpy fast paths.
			float base[16];
			if (a_haveBase) {
				std::memcpy(base, out, sizeof(base));
			}
			if (layer.mode == Mode::kAdditive) {
				const float weight = effectiveWeight * bone.maskWeight;
				PoseMath::WriteAdditive(out, base, layer.reference + offset, layer.sampled + offset, weight);
				a_drivenOut[joint] = weight > 0.0f;
			} else {
				const float* displayed = layer.displayed && layer.displayedDriven && layer.displayedDriven[joint] ?
					layer.displayed + offset : nullptr;
				const float strength = layer.poseWeight * bone.maskWeight;
				if (PoseMath::WriteOverrideTransition(out, a_haveBase ? base : nullptr,
					layer.sampled + offset, displayed, layer.transition, strength)) {
					a_drivenOut[joint] = strength > 0.0f || (displayed && (layer.transition < 1.0f || !a_haveBase));
				}
			}
		}
		return true;
	}
}
