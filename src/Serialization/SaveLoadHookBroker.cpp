#define OSF_SAVE_LOAD_HOOK_HOST 1
#include "Serialization/SaveLoadHookBroker.h"

#include "Util/Hooking.h"

#include <REL/ASM.h>

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace OSF::Serialization::SaveLoadHookBroker
{
    namespace
    {
        constexpr std::size_t kMaxListeners = 32;
        constexpr std::size_t kMaxListenerName = 96;

        constexpr std::array<std::uint8_t, 32> kSaveGameGate{
            0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48,
            0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
        };
        constexpr std::array<std::uint8_t, 31> kLoadGameGate{
            0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20, 0x44, 0x88, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10, 0x48,
            0x89, 0x48, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
        };

        using SaveGameFn = void(RE::BGSSaveLoadGame*, void*, void*, const char*);
        using LoadGameFn = bool(RE::BGSSaveLoadGame*, void*, bool, bool);

        SaveGameFn* g_originalSaveGame{ nullptr };
        LoadGameFn* g_originalLoadGame{ nullptr };

        std::atomic<bool> g_initializeAttempted{ false };
        std::atomic<bool> g_ready{ false };
        std::atomic<const OSFSaveLoadHookAPI*> g_activeProvider{ nullptr };
        std::atomic<std::uint64_t> g_saveSequence{ 0 };
        std::atomic<std::uint64_t> g_loadSequence{ 0 };

        struct ListenerRecord
        {
            std::uint32_t listenerID{ 0 };
            void* context{ nullptr };
            void (OSF_SAVE_LOAD_HOOK_CALL *onEvent)(
                void*, const OSFSaveLoadHookEventV1*){ nullptr };
            char name[kMaxListenerName]{};
        };

        void CopyText(char* a_destination, const std::size_t a_capacity, const char* a_source) noexcept
        {
            if (!a_destination || a_capacity == 0) {
                return;
            }
            a_destination[0] = '\0';
            if (!a_source) {
                return;
            }

            __try {
                std::size_t i = 0;
                for (; i < (a_capacity - 1) && a_source[i] != '\0'; ++i) {
                    a_destination[i] = a_source[i];
                }
                a_destination[i] = '\0';
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                a_destination[0] = '\0';
            }
        }

        template <class F>
        void SwallowAtHookBoundary(const char* a_label, F&& a_function) noexcept
        {
            try {
                std::forward<F>(a_function)();
            } catch (const std::exception& e) {
                try {
                    REX::CRITICAL("[Save] save/load broker {} threw '{}'; swallowed", a_label, e.what());
                } catch (...) {
                }
            } catch (...) {
                try {
                    REX::CRITICAL("[Save] save/load broker {} threw; swallowed", a_label);
                } catch (...) {
                }
            }
        }

        [[nodiscard]] bool ReadableRange(
            const std::uintptr_t a_address,
            const std::size_t a_size) noexcept
        {
            if (a_address == 0 || a_size == 0) {
                return false;
            }
            MEMORY_BASIC_INFORMATION memory{};
            if (::VirtualQuery(
                    reinterpret_cast<const void*>(a_address),
                    &memory,
                    sizeof(memory)) == 0 ||
                memory.State != MEM_COMMIT ||
                (memory.Protect & PAGE_GUARD) != 0 ||
                (memory.Protect & 0xFF) == PAGE_NOACCESS) {
                return false;
            }
            const auto regionEnd =
                reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
            return a_address <= regionEnd && a_size <= (regionEnd - a_address);
        }

        [[nodiscard]] bool EntryTargets(
            const std::uintptr_t a_entry,
            const std::uintptr_t a_thunk) noexcept
        {
            if (!ReadableRange(a_entry, 5)) {
                return false;
            }
            const auto* code = reinterpret_cast<const std::uint8_t*>(a_entry);
            if (code[0] != 0xE9) {
                return false;
            }

            std::int32_t displacement = 0;
            std::memcpy(&displacement, code + 1, sizeof(displacement));
            const auto branch = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(a_entry + 5) + displacement);
            if (branch == a_thunk) {
                return true;
            }
            if (!ReadableRange(branch, sizeof(REL::ASM::JMP14))) {
                return false;
            }
            const auto* island = reinterpret_cast<const std::uint8_t*>(branch);
            if (island[0] != 0xFF || island[1] != 0x25 ||
                island[2] != 0 || island[3] != 0 ||
                island[4] != 0 || island[5] != 0) {
                return false;
            }
            std::uintptr_t destination = 0;
            std::memcpy(&destination, island + 6, sizeof(destination));
            return destination == a_thunk;
        }

        class LocalBroker
        {
        public:
            static LocalBroker& GetSingleton() noexcept
            {
                static LocalBroker singleton;
                return singleton;
            }

            void SetReady(
                const std::uint32_t a_flags,
                const std::uintptr_t a_saveEntry,
                const std::uintptr_t a_saveThunk,
                const std::uintptr_t a_loadEntry,
                const std::uintptr_t a_loadThunk) noexcept
            {
                _saveEntry = a_saveEntry;
                _saveThunk = a_saveThunk;
                _loadEntry = a_loadEntry;
                _loadThunk = a_loadThunk;
                _flags.store(a_flags, std::memory_order_release);
                _ready.store(
                    (a_flags & OSF_SAVE_LOAD_HOOK_STATUS_REQUIRED) ==
                        OSF_SAVE_LOAD_HOOK_STATUS_REQUIRED,
                    std::memory_order_release);
            }

            [[nodiscard]] bool IsReady() const noexcept
            {
                return _ready.load(std::memory_order_acquire) && OwnsHooks();
            }

            [[nodiscard]] bool Register(const OSFSaveLoadHookListenerV1* a_listener)
            {
                if (!IsReady() || !a_listener ||
                    a_listener->size < sizeof(OSFSaveLoadHookListenerV1) ||
                    a_listener->listenerID == 0 || !a_listener->OnEvent) {
                    return false;
                }

                const std::scoped_lock lock{ _mutex };
                for (std::size_t i = 0; i < _listenerCount; ++i) {
                    const auto& existing = _listeners[i];
                    if (existing.listenerID == a_listener->listenerID) {
                        return existing.context == a_listener->context &&
                            existing.onEvent == a_listener->OnEvent;
                    }
                }
                if (_listenerCount >= _listeners.size()) {
                    REX::CRITICAL("[Save] save/load broker listener capacity exhausted ({})",
                                  _listeners.size());
                    return false;
                }

                auto& record = _listeners[_listenerCount++];
                record.listenerID = a_listener->listenerID;
                record.context = a_listener->context;
                record.onEvent = a_listener->OnEvent;
                CopyText(record.name, sizeof(record.name), a_listener->listenerName);
                REX::INFO("[Save] save/load broker listener registered id=0x{:08X} name='{}'",
                          record.listenerID,
                          record.name[0] != '\0' ? record.name : "<unnamed>");
                return true;
            }

            [[nodiscard]] bool Unregister(
                const std::uint32_t a_listenerID,
                void* a_context)
            {
                const std::scoped_lock lock{ _mutex };
                for (std::size_t i = 0; i < _listenerCount; ++i) {
                    if (_listeners[i].listenerID == a_listenerID &&
                        _listeners[i].context == a_context) {
                        for (std::size_t j = i + 1; j < _listenerCount; ++j) {
                            _listeners[j - 1] = _listeners[j];
                        }
                        _listeners[--_listenerCount] = {};
                        return true;
                    }
                }
                return false;
            }

            void Dispatch(const OSFSaveLoadHookEventV1& a_event) noexcept
            {
                std::array<ListenerRecord, kMaxListeners> listeners{};
                std::size_t count = 0;
                try {
                    const std::scoped_lock lock{ _mutex };
                    count = _listenerCount;
                    std::copy_n(_listeners.begin(), count, listeners.begin());
                } catch (...) {
                    try {
                        REX::CRITICAL("[Save] save/load broker snapshot failed phase={} sequence={}",
                                      a_event.phase, a_event.sequence);
                    } catch (...) {
                    }
                    return;
                }

                for (std::size_t i = 0; i < count; ++i) {
                    const auto listener = listeners[i];
                    SwallowAtHookBoundary("listener", [&] {
                        listener.onEvent(listener.context, &a_event);
                    });
                }
            }

            [[nodiscard]] std::uint32_t Flags() const noexcept
            {
                auto flags = _flags.load(std::memory_order_acquire);
                if (!OwnsHooks()) {
                    flags &= ~(OSF_SAVE_LOAD_HOOK_STATUS_SAVE_HOOK |
                               OSF_SAVE_LOAD_HOOK_STATUS_LOAD_HOOK);
                }
                return flags;
            }

        private:
            [[nodiscard]] bool OwnsHooks() const noexcept
            {
                return _saveEntry != 0 && _saveThunk != 0 &&
                    _loadEntry != 0 && _loadThunk != 0 &&
                    EntryTargets(_saveEntry, _saveThunk) &&
                    EntryTargets(_loadEntry, _loadThunk);
            }

            std::mutex _mutex;
            std::array<ListenerRecord, kMaxListeners> _listeners{};
            std::size_t _listenerCount{ 0 };
            std::atomic<bool> _ready{ false };
            std::atomic<std::uint32_t> _flags{ 0 };
            std::uintptr_t _saveEntry{ 0 };
            std::uintptr_t _saveThunk{ 0 };
            std::uintptr_t _loadEntry{ 0 };
            std::uintptr_t _loadThunk{ 0 };
        };

        std::uint8_t OSF_SAVE_LOAD_HOOK_CALL APIIsReady(const OSFSaveLoadHookAPI* a_api)
        {
            return a_api && a_api->context &&
                static_cast<LocalBroker*>(a_api->context)->IsReady();
        }

        std::uint8_t OSF_SAVE_LOAD_HOOK_CALL APIRegister(
            const OSFSaveLoadHookAPI* a_api,
            const OSFSaveLoadHookListenerV1* a_listener)
        {
            return a_api && a_api->context &&
                static_cast<LocalBroker*>(a_api->context)->Register(a_listener);
        }

        std::uint8_t OSF_SAVE_LOAD_HOOK_CALL APIUnregister(
            const OSFSaveLoadHookAPI* a_api,
            const std::uint32_t a_listenerID,
            void* a_context)
        {
            return a_api && a_api->context &&
                static_cast<LocalBroker*>(a_api->context)->Unregister(
                    a_listenerID, a_context);
        }

        std::uint8_t OSF_SAVE_LOAD_HOOK_CALL APIGetStatus(
            const OSFSaveLoadHookAPI* a_api,
            OSFSaveLoadHookStatusV1* a_status)
        {
            if (!a_api || !a_api->context || !a_status ||
                a_status->size < sizeof(OSFSaveLoadHookStatusV1)) {
                return 0;
            }
            a_status->flags = static_cast<LocalBroker*>(a_api->context)->Flags();
            a_status->providerName = "OSF Animation";
            return 1;
        }

        std::uint32_t OSF_SAVE_LOAD_HOOK_CALL APIVersion(const OSFSaveLoadHookAPI*)
        {
            return OSF_SAVE_LOAD_HOOK_API_VERSION;
        }

        [[nodiscard]] const OSFSaveLoadHookAPI& LocalAPITable()
        {
            static const OSFSaveLoadHookAPI api{
                sizeof(OSFSaveLoadHookAPI),
                &LocalBroker::GetSingleton(),
                &APIIsReady,
                &APIRegister,
                &APIUnregister,
                &APIGetStatus,
                &APIVersion,
            };
            return api;
        }

        [[nodiscard]] bool CompatibleAPI(const OSFSaveLoadHookAPI* a_api)
        {
            return a_api && a_api->size >= sizeof(OSFSaveLoadHookAPI) &&
                a_api->IsReady && a_api->RegisterListener &&
                a_api->UnregisterListener && a_api->GetStatus &&
                a_api->GetInterfaceVersion && a_api->IsReady(a_api) &&
                OSF_SAVE_LOAD_HOOK_API_MAJOR(a_api->GetInterfaceVersion(a_api)) ==
                    OSF_SAVE_LOAD_HOOK_API_MAJOR(OSF_SAVE_LOAD_HOOK_API_VERSION);
        }

        struct ProviderInfo
        {
            const OSFSaveLoadHookAPI* api{ nullptr };
            OSFSaveLoadHookStatusV1 status{};
            std::string name;
        };

        [[nodiscard]] std::optional<ProviderInfo> FindExternalProvider()
        {
            HMODULE selfModule = nullptr;
            (void)::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&g_activeProvider),
                &selfModule);

            const HANDLE snapshot = ::CreateToolhelp32Snapshot(
                TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                ::GetCurrentProcessId());
            if (snapshot == INVALID_HANDLE_VALUE) {
                return std::nullopt;
            }

            MODULEENTRY32W module{};
            module.dwSize = sizeof(module);
            bool haveModule = ::Module32FirstW(snapshot, &module) != FALSE;
            while (haveModule) {
                if (module.hModule != selfModule) {
                    const auto request = reinterpret_cast<OSF_RequestSaveLoadHookAPI_t>(
                        ::GetProcAddress(module.hModule, "OSF_RequestSaveLoadHookAPI"));
                    if (request) {
                        const OSFSaveLoadHookAPI* api = nullptr;
                        try {
                            api = request(OSF_SAVE_LOAD_HOOK_API_VERSION);
                        } catch (...) {
                            api = nullptr;
                        }
                        if (CompatibleAPI(api)) {
                            OSFSaveLoadHookStatusV1 status{
                                .size = sizeof(OSFSaveLoadHookStatusV1),
                            };
                            if (api->GetStatus(api, &status) &&
                                (status.flags & OSF_SAVE_LOAD_HOOK_STATUS_REQUIRED) ==
                                    OSF_SAVE_LOAD_HOOK_STATUS_REQUIRED) {
                                ProviderInfo result{
                                    .api = api,
                                    .status = status,
                                    .name = status.providerName ? status.providerName : "<unnamed>",
                                };
                                ::CloseHandle(snapshot);
                                return result;
                            }
                        }
                    }
                }
                haveModule = ::Module32NextW(snapshot, &module) != FALSE;
            }
            ::CloseHandle(snapshot);
            return std::nullopt;
        }

        template <std::size_t N>
        [[nodiscard]] bool VerifyExpectedBytes(
            const char* a_label,
            const std::uintptr_t a_address,
            const std::array<std::uint8_t, N>& a_expected)
        {
            const auto* actual = reinterpret_cast<const std::uint8_t*>(a_address);
            if (std::equal(a_expected.begin(), a_expected.end(), actual)) {
                return true;
            }

            std::string actualBytes;
            for (std::size_t i = 0; i < N; ++i) {
                if (i != 0) {
                    actualBytes += ' ';
                }
                actualBytes += std::format("{:02X}", actual[i]);
            }
            REX::WARN("[Save] {} bytes drifted at 0x{:X}: {}",
                      a_label, a_address, actualBytes);
            return false;
        }

        template <class Fn>
        [[nodiscard]] Fn* MakeGateway(
            const std::uintptr_t a_target,
            const std::size_t a_stolenBytes)
        {
            auto& trampoline = REL::GetTrampoline();
            auto* code = static_cast<std::byte*>(
                trampoline.allocate(a_stolenBytes + sizeof(REL::ASM::JMP14)));
            std::memcpy(code, reinterpret_cast<const void*>(a_target), a_stolenBytes);
            const REL::ASM::JMP14 jump{ a_target + a_stolenBytes };
            std::memcpy(code + a_stolenBytes, &jump, sizeof(jump));
            return reinterpret_cast<Fn*>(code);
        }

        [[nodiscard]] std::string BoundedName(const char* a_name)
        {
            if (!a_name) {
                return {};
            }
            const std::size_t length = ::strnlen(a_name, 512);
            return length < 512 ? std::string(a_name, length) : std::string{};
        }

        void SaveHook(
            RE::BGSSaveLoadGame* a_self,
            void* a_context,
            void* a_writer,
            const char* a_name) noexcept
        {
            const auto sequence = g_saveSequence.fetch_add(1, std::memory_order_relaxed) + 1;
            std::string name;
            SwallowAtHookBoundary("SAVE name capture", [&] {
                name = BoundedName(a_name);
            });
            OSFSaveLoadHookEventV1 event{
                .size = sizeof(OSFSaveLoadHookEventV1),
                .phase = OSF_SAVE_LOAD_HOOK_PHASE_SAVE_ENTRY,
                .sequence = sequence,
                .name = name.c_str(),
            };
            LocalBroker::GetSingleton().Dispatch(event);

            g_originalSaveGame(a_self, a_context, a_writer, a_name);

            event.phase = OSF_SAVE_LOAD_HOOK_PHASE_SAVE_RETURN;
            LocalBroker::GetSingleton().Dispatch(event);
        }

        bool LoadHook(
            RE::BGSSaveLoadGame* a_self,
            void* a_reader,
            const bool a_flag1,
            const bool a_flag2) noexcept
        {
            const auto sequence = g_loadSequence.fetch_add(1, std::memory_order_relaxed) + 1;
            std::string name;
            SwallowAtHookBoundary("LOAD name capture", [&] {
                name = BoundedName(static_cast<const char*>(a_reader));
            });
            OSFSaveLoadHookEventV1 event{
                .size = sizeof(OSFSaveLoadHookEventV1),
                .phase = OSF_SAVE_LOAD_HOOK_PHASE_LOAD_ENTRY,
                .sequence = sequence,
                .name = name.c_str(),
            };
            LocalBroker::GetSingleton().Dispatch(event);

            const bool result = g_originalLoadGame(a_self, a_reader, a_flag1, a_flag2);

            event.phase = OSF_SAVE_LOAD_HOOK_PHASE_LOAD_RETURN;
            event.flags = OSF_SAVE_LOAD_HOOK_EVENT_RESULT_VALID;
            event.result = result;
            LocalBroker::GetSingleton().Dispatch(event);
            return result;
        }

        [[nodiscard]] bool InstallDirectProvider()
        {
            const auto saveTarget = RE::ID::BGSSaveLoadGame::SaveGame.address();
            const auto loadTarget = RE::ID::BGSSaveLoadGame::LoadGame.address();
            const bool saveGate = VerifyExpectedBytes(
                "SaveGame 98376 full gate", saveTarget, kSaveGameGate);
            const bool loadGate = VerifyExpectedBytes(
                "LoadGame 98380 full gate", loadTarget, kLoadGameGate);
            if (!saveGate || !loadGate) {
                REX::CRITICAL("[Save] save/load broker direct validation failed save={} load={}",
                              saveGate, loadGate);
                return false;
            }

            auto& trampoline = REL::GetTrampoline();
            g_originalSaveGame = MakeGateway<SaveGameFn>(saveTarget, 5);
            g_originalLoadGame = MakeGateway<LoadGameFn>(loadTarget, 7);
            if (g_originalSaveGame) {
                trampoline.write_jmp<5>(saveTarget, &SaveHook);
            }
            if (g_originalLoadGame) {
                trampoline.write_jmp<5>(loadTarget, &LoadHook);
            }

            const std::uint32_t flags =
                OSF_SAVE_LOAD_HOOK_STATUS_SAVE_GATE |
                OSF_SAVE_LOAD_HOOK_STATUS_LOAD_GATE |
                (g_originalSaveGame ? OSF_SAVE_LOAD_HOOK_STATUS_SAVE_HOOK : 0u) |
                (g_originalLoadGame ? OSF_SAVE_LOAD_HOOK_STATUS_LOAD_HOOK : 0u);
            LocalBroker::GetSingleton().SetReady(
                flags,
                saveTarget,
                reinterpret_cast<std::uintptr_t>(&SaveHook),
                loadTarget,
                reinterpret_cast<std::uintptr_t>(&LoadHook));
            if (!LocalBroker::GetSingleton().IsReady()) {
                REX::CRITICAL("[Save] save/load broker direct hook pair incomplete");
                return false;
            }

            g_activeProvider.store(&LocalAPITable(), std::memory_order_release);
            REX::INFO("[Save] save/load broker ready provider='OSF Animation' mode=direct saveGate=true saveHook=true loadGate=true loadHook=true");
            return true;
        }

        [[nodiscard]] bool RegisterListener(
            const OSFSaveLoadHookAPI* a_provider,
            const OSFSaveLoadHookListenerV1& a_listener,
            const char* a_providerName)
        {
            if (!a_provider->RegisterListener(a_provider, &a_listener)) {
                REX::CRITICAL("[Save] save/load broker provider '{}' rejected internal listener",
                              a_providerName);
                return false;
            }
            REX::INFO("[Save] save/load broker ready provider='{}' mode=consumer saveGate=true saveHook=true loadGate=true loadHook=true",
                      a_providerName);
            return true;
        }
    }

    bool Initialize(const OSFSaveLoadHookListenerV1& a_listener)
    {
        if (g_initializeAttempted.exchange(true, std::memory_order_acq_rel)) {
            return g_ready.load(std::memory_order_acquire);
        }

        bool ready = false;
        try {
            if (const auto provider = FindExternalProvider()) {
                if (RegisterListener(provider->api, a_listener, provider->name.c_str())) {
                    g_activeProvider.store(provider->api, std::memory_order_release);
                    ready = true;
                }
            } else if (InstallDirectProvider()) {
                const auto* activeProvider = g_activeProvider.load(std::memory_order_acquire);
                ready = activeProvider && RegisterListener(activeProvider, a_listener, "OSF Animation");
            }
        } catch (const std::exception& e) {
            REX::CRITICAL("[Save] save/load broker initialization threw '{}'; fail closed", e.what());
        } catch (...) {
            REX::CRITICAL("[Save] save/load broker initialization threw; fail closed");
        }

        g_ready.store(ready, std::memory_order_release);
        return ready;
    }

    const OSFSaveLoadHookAPI* RequestAPI(const std::uint32_t a_requestedVersion)
    {
        if (OSF_SAVE_LOAD_HOOK_API_MAJOR(a_requestedVersion) !=
                OSF_SAVE_LOAD_HOOK_API_MAJOR(OSF_SAVE_LOAD_HOOK_API_VERSION) ||
            OSF_SAVE_LOAD_HOOK_API_MINOR(a_requestedVersion) >
                OSF_SAVE_LOAD_HOOK_API_MINOR(OSF_SAVE_LOAD_HOOK_API_VERSION)) {
            return nullptr;
        }
        const auto* provider = g_activeProvider.load(std::memory_order_acquire);
        return CompatibleAPI(provider) ? provider : nullptr;
    }
}

OSF_SAVE_LOAD_HOOK_EXPORT const OSFSaveLoadHookAPI* OSF_SAVE_LOAD_HOOK_CALL
    OSF_RequestSaveLoadHookAPI(const std::uint32_t a_requestedVersion)
{
    return OSF::Serialization::SaveLoadHookBroker::RequestAPI(a_requestedVersion);
}
