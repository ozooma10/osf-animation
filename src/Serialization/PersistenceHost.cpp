#include "Serialization/PersistenceHost.h"

#define OSF_PERSISTENCE_HOST 1
#include "API/OSFPersistenceAPI.h"
#include "Animation/GraphManager.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#undef ERROR

namespace OSF::Serialization::PersistenceHost
{
    namespace
    {
        std::atomic_bool g_registered{};
        std::atomic_bool g_fallbackReverted{};
        std::mutex g_registrationMutex;

        const OSFPersistenceAPI* Resolve(std::uint32_t version)
        {
            const auto module = ::GetModuleHandleW(L"OSFSettings.dll");
            if (!module) return nullptr;
            const auto request = reinterpret_cast<OSF_RequestPersistenceAPI_t>(
                ::GetProcAddress(module, "OSF_RequestPersistenceAPI"));
            return request ? request(version) : nullptr;
        }
    }

    void RegisterClient()
    {
        std::lock_guard lock(g_registrationMutex);
        if (g_registered.load()) return;
        const auto* api = Resolve(OSF_PERSISTENCE_API_VERSION);
        if (!api || api->size < sizeof(OSFPersistenceAPI) || !api->IsReady(api)) {
            REX::WARN("[Save] OSF Settings persistence unavailable; Animation retains local load teardown");
            return;
        }
        OSFPersistenceClientV1 client{};
        client.size = sizeof(client);
        client.clientID = OSF_PERSISTENCE_FOURCC('O', 'S', 'F', 'A');
        client.clientName = "OSF Animation";
        client.save = [](void*, OSFRecordWriterV1*) -> std::uint8_t { return 1; };
        client.load = [](void*, OSFRecordReaderV1*) -> std::uint8_t { return 1; };
        client.revert = [](void*) { Animation::GraphManager::GetSingleton().StopAll("persistence revert (OSFA)"); };
        if (!api->RegisterClient(api, &client)) {
            REX::ERROR("[Save] could not register OSFA with OSF Settings; retaining local load teardown");
            return;
        }
        g_registered.store(true);
        REX::INFO("[Save] Animation registered with OSF Settings persistence");
    }

    void BeginLoad(const char* reason)
    {
        if (!g_registered.load() && !g_fallbackReverted.exchange(true)) {
            Animation::GraphManager::GetSingleton().StopAll(reason);
        }
    }

    void AbortLoad()
    {
        if (!g_registered.load()) g_fallbackReverted.store(false);
    }

    void OnLoadBackstop()
    {
        if (!g_registered.load()) {
            if (!g_fallbackReverted.exchange(false)) {
                Animation::GraphManager::GetSingleton().StopAll("load/new-game backstop (no persistence host)");
            }
        }
    }

    const OSFPersistenceAPI* RequestAPI(std::uint32_t version) { return Resolve(version); }
}

// Existing consumers can continue to resolve this symbol from Animation.
// Both exports return the same Settings-owned table; Animation installs no save hooks.
OSF_PERSISTENCE_EXPORT const OSFPersistenceAPI* OSF_PERSISTENCE_CALL
OSF_RequestPersistenceAPI(std::uint32_t version)
{
    return OSF::Serialization::PersistenceHost::RequestAPI(version);
}
