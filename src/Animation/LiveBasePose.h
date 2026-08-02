#pragma once

// Engine-free helpers for preserving the immutable vanilla pose used underneath
// additive and feathered-mask playback. A Starfield 3D rebuild may remap rig slots,
// so the carry-over cache is keyed by the stable ozz skeleton joint index instead.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace OSF::Animation::LiveBasePose
{
	inline constexpr std::size_t kTransformFloats = 16;

	class Cache
	{
	public:
		void Clear()
		{
			_poseByJoint.clear();
			_validByJoint.clear();
		}

		template <class Binding, std::size_t Extent>
		void Store(std::span<Binding, Extent> a_binding, std::span<const float> a_pose,
			std::size_t a_jointCount)
		{
			if (a_pose.size() != a_binding.size() * kTransformFloats) {
				return;
			}
			if (_validByJoint.size() != a_jointCount) {
				_poseByJoint.assign(a_jointCount * kTransformFloats, 0.0f);
				_validByJoint.assign(a_jointCount, 0);
			}
			std::fill(_validByJoint.begin(), _validByJoint.end(), std::uint8_t{ 0 });
			for (std::size_t i = 0; i < a_binding.size(); ++i) {
				const auto joint = static_cast<std::size_t>(a_binding[i].jointIndex);
				if (joint >= a_jointCount) {
					continue;
				}
				std::memcpy(_poseByJoint.data() + joint * kTransformFloats,
					a_pose.data() + i * kTransformFloats,
					kTransformFloats * sizeof(float));
				_validByJoint[joint] = 1;
			}
		}

		template <class Binding, std::size_t Extent>
		[[nodiscard]] bool Restore(std::span<Binding, Extent> a_binding,
			std::span<float> a_pose) const
		{
			if (a_binding.empty() || a_pose.size() != a_binding.size() * kTransformFloats) {
				return false;
			}
			for (const auto& bound : a_binding) {
				const auto joint = static_cast<std::size_t>(bound.jointIndex);
				if (joint >= _validByJoint.size() || !_validByJoint[joint]) {
					return false;
				}
			}
			for (std::size_t i = 0; i < a_binding.size(); ++i) {
				const auto joint = static_cast<std::size_t>(a_binding[i].jointIndex);
				std::memcpy(a_pose.data() + i * kTransformFloats,
					_poseByJoint.data() + joint * kTransformFloats,
					kTransformFloats * sizeof(float));
			}
			return true;
		}

	private:
		std::vector<float> _poseByJoint;
		std::vector<std::uint8_t> _validByJoint;
	};

	// An AnimationManager update proves freshness only for the binding generation
	// that existed after that engine evaluation. A compose-time rebind increments
	// the generation and must not inherit the old authorization.
	class Evaluation
	{
	public:
		void Clear()
		{
			_engineRevision = 0;
			_bindingRevision = 0;
		}

		void Mark(std::uint64_t a_engineRevision, std::uint64_t a_bindingRevision)
		{
			_engineRevision = a_engineRevision;
			_bindingRevision = a_bindingRevision;
		}

		[[nodiscard]] bool IsCurrent(std::uint64_t a_engineRevision,
			std::uint64_t a_bindingRevision) const
		{
			return _engineRevision == a_engineRevision &&
			       _bindingRevision == a_bindingRevision;
		}

	private:
		std::uint64_t _engineRevision = 0;
		std::uint64_t _bindingRevision = 0;
	};
}
