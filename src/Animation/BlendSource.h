#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace OSF::Animation::BlendSource
{
	// Pose handoff from a graph being torn down: teardown invalidates the rig binding, so capture
	// the pose and its driven joints together.
	template <class Pose>
	class Prepared
	{
	public:
		template <class Binding>
		bool Capture(const std::vector<Pose>& a_pose, const std::vector<Binding>& a_binding)
		{
			Clear();
			if (a_pose.empty() || a_binding.empty()) {
				return false;
			}

			_pose = a_pose;
			_driven.assign(a_pose.size(), 0);
			for (const auto& bound : a_binding) {
				if (bound.jointIndex < _driven.size()) {
					_driven[bound.jointIndex] = 1;
					_prepared = true;
				}
			}
			if (!_prepared) {
				Clear();
			}
			return _prepared;
		}

		bool Consume(std::size_t a_expectedJointCount, std::vector<Pose>& a_pose,
			std::vector<std::uint8_t>& a_driven)
		{
			if (!_prepared) {
				return false;
			}
			if (_pose.size() != a_expectedJointCount || _driven.size() != a_expectedJointCount) {
				Clear();
				return false;
			}

			a_pose = std::move(_pose);
			a_driven = std::move(_driven);
			_prepared = false;
			return true;
		}

		void Clear()
		{
			_pose.clear();
			_driven.clear();
			_prepared = false;
		}

	private:
		std::vector<Pose> _pose;
		std::vector<std::uint8_t> _driven;
		bool _prepared = false;
	};
}
