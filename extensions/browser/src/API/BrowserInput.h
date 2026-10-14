#pragma once
#include "OSFEngineAPI.h"

namespace OSF::API::BrowserInput
{
	inline void SetVisible(const OSFEngineAPI& engine, bool visible) { engine.SetInputRouting(visible ? SuppressRawMouse | ConsumeThumbsticks | ConsumeGamepadAfterDispatch | AllowNativeFreeCamera : 0); }
	inline void Orbit(const OSFEngineAPI& engine, float dx, float dy, float wheel)
	{
		// OSF UI supplies logical pixels. Keep the existing browser sensitivity in the addon.
		constexpr float dragScale = 0.6f;
		engine.InjectOrbitDelta(dx * dragScale, dy * dragScale, wheel);
	}
}
