#pragma once

#include <cmath>
#include <numbers>

namespace OSF::Util
{
	inline constexpr float  kPi  = std::numbers::pi_v<float>;
	inline constexpr double kPiD = std::numbers::pi_v<double>;
	inline constexpr double kDegToRad = kPiD / 180.0;
	inline constexpr float kDegToRadF = static_cast<float>(kDegToRad);
}
