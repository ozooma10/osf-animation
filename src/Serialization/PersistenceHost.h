#pragma once

namespace OSF::Serialization::PersistenceHost
{
    // Registers Animation as OSFA with OSF Settings at kPostDataLoad.
    void RegisterClient();

    // Local teardown fallback when Settings is absent/unavailable. When registered,
    // Settings owns the lifecycle and invokes OSFA's revert callback.
    void BeginLoad(const char* reason);
    void AbortLoad();
    void OnLoadBackstop();
}
