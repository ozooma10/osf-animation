#pragma once

namespace OSF::Maintenance
{
	// Installs one process-lifetime SFSE producer. Its worker notifications are coalesced onto the verified BSService main-thread drain.
	bool InstallFrameMaintenance();
}
