#pragma once
#ifdef OSF_ANIMATION_SMOKE
#include <filesystem>
#endif

namespace OSF::Studio
{
	// Owned by Maintenance::FrameMaintenance; cheap early-out while Studio Link is disabled.
	void MaintenanceTick();

	// Game thread; idempotent. Disabling restores any preview the service owns.
	void SetPreviewServiceEnabled(bool a_enabled) noexcept;
#ifdef OSF_ANIMATION_SMOKE
	// Isolates the real protocol monitor from the user's Studio mailbox.
	void SetDirectoryForSmoke(const std::filesystem::path& a_directory);
#endif
}
