#pragma once

namespace OSF::Studio
{
	// Enables or disables the per-session Studio Link inbox. Must be called on the game thread;
	// repeated calls are safe, and disabling restores any preview owned by the service.
	void SetPreviewServiceEnabled(bool a_enabled) noexcept;
}
