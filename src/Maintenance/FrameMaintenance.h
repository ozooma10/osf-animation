#pragma once

namespace OSF::Maintenance
{
	// Install once at plugin load. Runs queued commands and maintenance after UI_AdvanceActiveMenus.
	bool InstallFrameMaintenance();
}
