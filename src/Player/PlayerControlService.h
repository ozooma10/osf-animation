#pragma once

#include "Player/PlayerInputLockService.h"

namespace OSF::Player
{
	// Compatibility name retained for existing native callers. New code should include
	// PlayerInputLockService.h and use PlayerInputLockService directly.
	using PlayerControlService = PlayerInputLockService;
}
