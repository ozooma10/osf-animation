#pragma once

namespace OSF::Authoring
{
	// Explicit opt-in for this process only. IO stays on a worker; game operations use the frame queue.
	bool SetAlignmentToolsEnabled(bool a_enabled);
	void InitializeAlignmentTools();
}
