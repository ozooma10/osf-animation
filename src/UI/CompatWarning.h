#pragma once

namespace OSF::UI
{
	namespace CompatWarning
	{
		// Checks whether SAF or NAFSF are loaded alongside us (via GetModuleHandle, so we
		// don't load or ref-count them). If either is, logs it and pops a blocking message box.
		void ProbeIncompatibilities();
	}
}
