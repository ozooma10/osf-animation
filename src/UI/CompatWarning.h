#pragma once

namespace OSF::UI
{
	namespace CompatWarning
	{
		// Probes for co-loaded SAF/NAFSF modules (GetModuleHandle — no load, no ref count)
		// If any are present, logs it and raises a blocking Win32 MessageBox
		void ProbeIncompatibilities();
	}
}
