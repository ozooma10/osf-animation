#pragma once

#include "API/OSFSaveLoadHookAPI.h"

#include <cstdint>

namespace OSF::Serialization::SaveLoadHookBroker
{
    // Joins an already-published compatible provider, or installs the proven
    // SaveGame/LoadGame hooks and becomes the provider when none exists.
    [[nodiscard]] bool Initialize(const OSFSaveLoadHookListenerV2& a_listener);

    // Returned by the public C export. This may forward another module's API
    // when that module won provider ownership earlier in process startup.
    [[nodiscard]] const OSFSaveLoadHookAPI* RequestAPI(std::uint32_t a_requestedVersion);
}
