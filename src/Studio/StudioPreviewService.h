#pragma once

namespace OSF::Studio
{
	// Creates the per-session Studio Link inbox and starts its file monitor. Safe to call more than once.
	void StartPreviewService();
}
