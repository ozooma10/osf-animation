#pragma once

namespace OSF::Studio
{
	// Main-thread pump owned by Maintenance::FrameMaintenance. Cheap atomic early-out while
	// Studio Link is disabled.
	void MaintenanceTick();

	// Enables or disables the per-session Studio Link inbox. Must be called on the game thread;
	// repeated calls are safe, and disabling restores any preview owned by the service.
	void SetPreviewServiceEnabled(bool a_enabled) noexcept;
}
