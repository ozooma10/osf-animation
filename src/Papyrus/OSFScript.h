#pragma once

namespace OSF::Papyrus
{
	// The papyrus script name the PUBLIC natives are bound to. Callable from the
	// console:
	//   cgf "OSF.Play" player "OSF\test.glb"
	//   cgf "OSF.Stop" player
	inline constexpr std::string_view SCRIPT_NAME = "OSF";

	// Separate script name for COMPATIBILITY-ONLY natives — escape hatches the
	// SAF->OSF shim (SAFScript.psc) needs to reproduce SAF behavior on the
	// primitive (non-Scene) playback path (standalone player control/camera
	// locks). They are deliberately NOT on the OSF public surface. Scene-
	// integrated control/camera policy lives in the OSF Intimacy scene engine.
	// See OSFCompat.psc.
	inline constexpr std::string_view COMPAT_SCRIPT_NAME = "OSFCompat";

	void RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm);

	// Convenience overload: grabs the current GameVM and (re)binds every native,
	// returning false if the VM is unavailable. Call this at kPostDataLoad AND
	// after every save load: Starfield tears down and rebuilds the Papyrus VM on
	// load (the script log visibly closes and reopens), which drops the natives
	// bound once at kPostDataLoad. If they are not re-bound onto the fresh VM,
	// every OSF.* / OSFCompat.* call (including the SAF->OSF shim's) fails with
	// "Unbound native function" and playback silently no-ops. See SaveSafety's
	// TESLoadGameEvent sink.
	bool RegisterFunctions();
}
