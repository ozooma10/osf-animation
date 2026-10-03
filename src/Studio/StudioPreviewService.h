#pragma once

namespace OSF::Studio
{
	// Owned by Maintenance::FrameMaintenance; cheap early-out while Studio Link is disabled.
	void MaintenanceTick();

	// Game thread; idempotent. Disabling restores any preview the service owns.
	void SetPreviewServiceEnabled(bool a_enabled) noexcept;
}
