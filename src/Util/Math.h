#pragma once

#include <cmath>
#include <numbers>

namespace OSF::Util
{
	// kPi is used by scene-math tests; kDegToRad/kDegToRadF convert authored degrees to runtime radians.
	inline constexpr float  kPi  = std::numbers::pi_v<float>;
	inline constexpr double kPiD = std::numbers::pi_v<double>;
	inline constexpr double kDegToRad = kPiD / 180.0;  // pi/180
	inline constexpr float kDegToRadF = static_cast<float>(kDegToRad);
}
