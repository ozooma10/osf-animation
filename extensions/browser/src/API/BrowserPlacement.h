#pragma once
#include "OSFEngineAPI.h"

namespace OSF::API::BrowserPlacement
{
	bool InFrontOfView(RE::Actor* actor, float distance, OSFStartOptions& options);
}
