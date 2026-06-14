#pragma once

#include <cmath>
#include <numbers>

namespace OSF::Util
{
	inline constexpr float  kPi  = std::numbers::pi_v<float>;
	inline constexpr double kPiD = std::numbers::pi_v<double>;
	inline constexpr double kDegToRad = kPiD / 180.0;  // pi/180
	inline constexpr double kRadToDeg = 180.0 / kPiD;     // 180/pi
	inline constexpr float kDegToRadF = static_cast<float>(kDegToRad);
	inline constexpr float kRadToDegF = static_cast<float>(kRadToDeg);

	// Wrap an angle (radians) into (-pi, pi].
	inline float WrapRadians(float a_angle)
	{
		while (a_angle > kPi) {
			a_angle -= 2.0f * kPi;
		}
		while (a_angle < -kPi) {
			a_angle += 2.0f * kPi;
		}
		return a_angle;
	}
}
