#pragma once

#include "Input/InputTypes.h"

#include <functional>

namespace OSF::Input
{
	struct GamepadState
	{
		float leftX{ 0.0f };
		float leftY{ 0.0f };
		float rightX{ 0.0f };
		float rightY{ 0.0f };
		float leftTrigger{ 0.0f };
		float rightTrigger{ 0.0f };
		bool  leftShoulder{ false };
		bool  rightShoulder{ false };
	};

	// Vfunc swap on RE::UI's BSInputEventReceiver::PerformInputProcessing, which receives the per-frame raw InputEvent queue.
	class InputService
	{
	public:
		static InputService& GetSingleton();

		// Must pass before Install.
		bool VerifyUiLayout();

		// Once the UI singleton exists (kPostPostDataLoad). Idempotent.
		bool Install();

		// Game thread.
		void SetVerbHandler(std::function<void(Verb, const Grant&)> a_handler);

		// Driven by the undo ledger (Mechanism::kInputChannel). Release is a no-op unless a_handle is the active grant.
		void Engage(const Grant& a_grant);
		void Release(std::int32_t a_handle);

		// Scene-orbit passthrough; Drain* return and reset the accumulators. Wheel + = up / zoom in.
		void SetMouseCapture(bool a_on);
		void DrainMouseDelta(float& a_dx, float& a_dy);
		void DrainWheelDelta(float& a_wheel);
		[[nodiscard]] GamepadState ReadGamepadState() const;

		// Exclusive external channel, using API::InputRouting flags; zero restores normal routing.
        void SetInputRouting(std::uint32_t flags);

		// TFC reads the engine input path, so gamepad events pass through while it's active.
		void SetNativeFreeCamGamepad(bool a_on);

		// Mouse-equivalent deltas (wheel + = zoom in); dropped when capture is off.
		void InjectOrbitDelta(float a_dx, float a_dy, float a_wheel);

		// Load teardown.
		void OnStopAll();
	};
}
