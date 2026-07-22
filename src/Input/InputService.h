#pragma once

#include "Input/InputTypes.h"

#include <functional>

namespace OSF::Input
{
	// input hook: a vfunc swap on RE::UI's BSInputEventReceiver::PerformInputProcessing; the point where the game's UI receives the per-frame raw InputEvent queue. 
	class InputService
	{
	public:
		static InputService& GetSingleton();

		// Proves the compiled UI layout matches the running binary. Must pass before install. Logs + dumps on failure.
		bool VerifyUiLayout();

		// Swaps the vfunc. Call once the UI singleton exists (kPostPostDataLoad). Idempotent.
		bool Install();

		// True once the vfunc hook is installed.
		bool Installed() const;

		// The runtime registers HOW a verb executes (it drives the active scene + clock). Called on the game thread.
		void SetVerbHandler(std::function<void(Verb, const Grant&)> a_handler);

		// Engage/release the director channel for one scene. Engage stores the grant + arms dispatch;
		// Release clears it if a_handle is the active grant. Driven by the undo ledger (Mechanism::kInputChannel), so the channel tracks the scene lock's lifecycle exactly.
		void Engage(const Grant& a_grant);
		void Release(std::int32_t a_handle);

		// Raw mouse-look delta passthrough for the self-driven scene-orbit camera. While capture is on the
		// input hook accumulates MouseMoveEvent deltas; DrainMouseDelta returns + resets the accumulator.
		// The mouse WHEEL (carried as mouse ButtonEvents) is accumulated too for orbit zoom; DrainWheelDelta
		// returns + resets the net wheel ticks (+ = wheel up / zoom in).
		void SetMouseCapture(bool a_on);
		void DrainMouseDelta(float& a_dx, float& a_dy);
		void DrainWheelDelta(float& a_wheel);

		// A UI cursor is on screen (the scene browser reported visible via osf.animation.opened/osf.animation.closed).
		// While set, orbit capture only accumulates look/wheel deltas during an LMB DRAG, so plain
		// mouse movement drives the cursor and plain wheel scrolls the UI; while clear (no cursor),
		// the orbit free-looks as before. Sticky across scenes — it tracks the browser, not a scene.
		// NOTE: while the OSF UI overlay is open its WndProc swallow starves the input hook of
		// keyboard/mouse — the browser steers via InjectOrbitDelta instead. GAMEPAD input is polled
		// (XInput), not messaged, so it still reaches the hook; while this flag is set the hook
		// consumes gamepad events (status = kStop) so the player can't walk/jump under the browser.
		void SetUiCursorVisible(bool a_on);

		// Native free cam (`tfc`) owns the gamepad while active. The browser normally consumes
		// thumbstick events before the engine sees them (the self-driven orbit polls XInput), but
		// TFC uses the engine input path; allow those events through until free cam exits.
		void SetNativeFreeCamGamepad(bool a_on);

		// Bridge-fed orbit steering (osf.orbit from the scene browser): world-area LMB-drag deltas
		// in view CSS px (+wheel notches, + = zoom in). Scaled to the raw-mouse axis and added to
		// the same accumulators the hook feeds; dropped when no orbit capture is active.
		void InjectOrbitDelta(float a_dx, float a_dy, float a_wheel);

		// Load teardown: drop the grant + disarm. Mirrors Player/CameraService::OnStopAll.
		void OnStopAll();
	};
}
