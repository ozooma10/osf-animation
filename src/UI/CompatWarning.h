#pragma once

// Co-loaded-framework warning. OSF, SAF (StarfieldAnimationFramework.dll) and
// NAFSF (NAF.dll) all stamp the engine's flat rig buffer every frame; running
// two at once means they overwrite each other's poses and actors twitch /
// T-pose. We can't stop the other plugin from loading, so we surface it: detect
// the incompatible DLL and pop a plain Win32 MessageBox (the same kind
// CommonLibSF raises for an address-library-not-found error) so the user sees
// it before they ever start a scene.
//
// Why a Win32 box and NOT an in-game popup: the warning has to land at startup,
// but the Papyrus VM isn't up yet when the main menu opens (RE-observed: the
// MainMenu event fires ~2 s before the script log opens), so Debug.MessageBox
// silently no-ops, and a native MessageBoxMenu would need from-scratch RE.
// MessageBoxW needs neither the VM nor any engine binding and shows before the
// title screen even renders. See AGENTS.md "Co-load warning".

namespace OSF::UI
{
	namespace CompatWarning
	{
		// Probes for co-loaded SAF/NAFSF modules (GetModuleHandle — no load, no
		// ref count) and, if any are present, logs it and raises a blocking Win32
		// MessageBox. Always logs the verdict. No-op (beyond the log) when no
		// incompatible framework is loaded. MUST run at kPostLoad (or later): SFSE
		// loads plugins in directory order, so a probe at our own SFSE_PLUGIN_LOAD
		// misses one that loads after us. kPostLoad fires once every plugin's load
		// handler has run, so all incompatible DLLs are in-process by then.
		void ProbeIncompatibilities();
	}
}
