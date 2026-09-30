#pragma once

#include <algorithm>
#include <utility>
#include <vector>

namespace OSF::Scene
{
	// Reconcile at prop marks, rewinds, lifecycle changes, or an explicit seek.
	// Ordinary frames do no prop work; a failed attachment retries at the next change.
	class InspectionPropCursor
	{
	public:
		void Reset(std::vector<float> a_marks)
		{
			_marks = std::move(a_marks);
			std::ranges::sort(_marks);
			_initialized = false;
		}

		bool Update(float a_position, bool a_atEnd, bool a_force = false)
		{
			const auto count = static_cast<std::size_t>(
				std::upper_bound(_marks.begin(), _marks.end(), a_position) - _marks.begin());
			const bool changed = !_initialized || a_force || a_position < _position ||
				count != _markCount || a_atEnd != _atEnd;
			_initialized = true;
			_position = a_position;
			_markCount = count;
			_atEnd = a_atEnd;
			return changed;
		}

	private:
		std::vector<float> _marks;
		float _position = 0.0f;
		std::size_t _markCount = 0;
		bool _atEnd = false;
		bool _initialized = false;
	};
}
