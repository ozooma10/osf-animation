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

		// --- SAF-compat activate redirect ----------------------------------------------------------
		// Legacy SAF/NAF mods (e.g. SnuSnu) progress a scene by having the player "Talk to" a participant, While armed (SetCompatActivate(true), driven by the SAF-compat player lock),
		// the input hook catches the Activate key press and posts a_handler on the game thread to trigger the activation directly.
		void SetCompatActivateHandler(std::function<void()> a_handler);
		void SetCompatActivate(bool a_armed);

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

		// Load teardown: drop the grant + disarm. Mirrors Player/CameraService::OnStopAll.
		void OnStopAll();
	};
}
