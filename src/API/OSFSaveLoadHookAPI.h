// ============================================================================
// OSFSaveLoadHookAPI.h - copyable C ABI for cooperative Starfield save/load
// entry hooks.
//
// Drop this header into a native SFSE plugin and link nothing from any OSF mod.
// Enumerate loaded modules for OSF_RequestSaveLoadHookAPI. If a compatible,
// ready provider exists, register a listener. If none exists, a plugin may
// install its own fully validated hooks and publish this same API.
//
// Providers invoke SAVE_ENTRY before the engine SaveGame gateway. Every
// listener must return nonzero to allow serialization; zero vetoes the gateway.
// Providers invoke SAVE_RETURN after the gateway returns or after a veto is
// finalized, and invoke LOAD_RETURN after the load gateway returns.
// Listener callbacks are bounded to the dispatch call, may run off the game
// thread, and must not throw across this ABI. Providers catch defensively.
// ============================================================================

#pragma once

#include <stdint.h>

#if defined(_WIN32)
#  define OSF_SAVE_LOAD_HOOK_CALL __cdecl
#  if defined(OSF_SAVE_LOAD_HOOK_HOST)
#    define OSF_SAVE_LOAD_HOOK_EXPORT __declspec(dllexport)
#  else
#    define OSF_SAVE_LOAD_HOOK_EXPORT
#  endif
#else
#  define OSF_SAVE_LOAD_HOOK_CALL
#  define OSF_SAVE_LOAD_HOOK_EXPORT
#endif

#define OSF_SAVE_LOAD_HOOK_MAKE_VERSION(major, minor) ((((uint32_t)(major)) << 16u) | ((uint32_t)(minor) & 0xFFFFu))
#define OSF_SAVE_LOAD_HOOK_API_VERSION OSF_SAVE_LOAD_HOOK_MAKE_VERSION(2u, 0u)
#define OSF_SAVE_LOAD_HOOK_API_MAJOR(version) ((uint32_t)(version) >> 16u)
#define OSF_SAVE_LOAD_HOOK_API_MINOR(version) ((uint32_t)(version) & 0xFFFFu)
#define OSF_SAVE_LOAD_HOOK_FOURCC(a, b, c, d) \
    ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8u) | \
     ((uint32_t)(uint8_t)(c) << 16u) | ((uint32_t)(uint8_t)(d) << 24u))

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OSFSaveLoadHookEventV1 OSFSaveLoadHookEventV1;
typedef struct OSFSaveLoadHookListenerV2 OSFSaveLoadHookListenerV2;
typedef struct OSFSaveLoadHookStatusV1 OSFSaveLoadHookStatusV1;
typedef struct OSFSaveLoadHookAPI OSFSaveLoadHookAPI;

enum OSFSaveLoadHookPhaseV1
{
    OSF_SAVE_LOAD_HOOK_PHASE_SAVE_ENTRY = 1,
    OSF_SAVE_LOAD_HOOK_PHASE_SAVE_RETURN = 2,
    OSF_SAVE_LOAD_HOOK_PHASE_LOAD_ENTRY = 3,
    OSF_SAVE_LOAD_HOOK_PHASE_LOAD_RETURN = 4
};

enum OSFSaveLoadHookEventFlagsV1
{
    OSF_SAVE_LOAD_HOOK_EVENT_RESULT_VALID = 1u << 0u,
    OSF_SAVE_LOAD_HOOK_EVENT_SAVE_VETOED = 1u << 1u
};

enum OSFSaveLoadHookStatusFlagsV1
{
    OSF_SAVE_LOAD_HOOK_STATUS_SAVE_GATE = 1u << 0u,
    OSF_SAVE_LOAD_HOOK_STATUS_SAVE_HOOK = 1u << 1u,
    OSF_SAVE_LOAD_HOOK_STATUS_LOAD_GATE = 1u << 2u,
    OSF_SAVE_LOAD_HOOK_STATUS_LOAD_HOOK = 1u << 3u,
    OSF_SAVE_LOAD_HOOK_STATUS_SAVE_VETO = 1u << 4u
};

#define OSF_SAVE_LOAD_HOOK_STATUS_REQUIRED \
    (OSF_SAVE_LOAD_HOOK_STATUS_SAVE_GATE | OSF_SAVE_LOAD_HOOK_STATUS_SAVE_HOOK | \
     OSF_SAVE_LOAD_HOOK_STATUS_LOAD_GATE | OSF_SAVE_LOAD_HOOK_STATUS_LOAD_HOOK | \
     OSF_SAVE_LOAD_HOOK_STATUS_SAVE_VETO)

struct OSFSaveLoadHookEventV1
{
    uint32_t size;
    uint32_t phase;
    uint32_t flags;
    uint32_t reserved;
    uint64_t sequence;
    uint64_t result;
    const char* name;  // Valid only for the duration of the callback.
};

struct OSFSaveLoadHookListenerV2
{
    uint32_t size;
    uint32_t listenerID;       // Stable, globally unique FourCC-style identifier.
    const char* listenerName;  // UTF-8 display name; copied during registration.
    void* context;             // Consumer-owned; must outlive registration.
    // SAVE_ENTRY: nonzero allows serialization; zero vetoes it. The return
    // value is ignored for all other phases.
    uint8_t (OSF_SAVE_LOAD_HOOK_CALL *OnEvent)(
        void* context, const OSFSaveLoadHookEventV1* event);
};

struct OSFSaveLoadHookStatusV1
{
    uint32_t size;
    uint32_t flags;
    const char* providerName;  // Provider-owned static UTF-8 string.
};

struct OSFSaveLoadHookAPI
{
    uint32_t size;
    void* context;

    uint8_t (OSF_SAVE_LOAD_HOOK_CALL *IsReady)(const OSFSaveLoadHookAPI* api);
    uint8_t (OSF_SAVE_LOAD_HOOK_CALL *RegisterListener)(
        const OSFSaveLoadHookAPI* api, const OSFSaveLoadHookListenerV2* listener);
    uint8_t (OSF_SAVE_LOAD_HOOK_CALL *UnregisterListener)(
        const OSFSaveLoadHookAPI* api, uint32_t listenerID, void* context);
    uint8_t (OSF_SAVE_LOAD_HOOK_CALL *GetStatus)(
        const OSFSaveLoadHookAPI* api, OSFSaveLoadHookStatusV1* outStatus);
    uint32_t (OSF_SAVE_LOAD_HOOK_CALL *GetInterfaceVersion)(const OSFSaveLoadHookAPI* api);
};

typedef const OSFSaveLoadHookAPI* (OSF_SAVE_LOAD_HOOK_CALL *OSF_RequestSaveLoadHookAPI_t)(
    uint32_t requestedVersion);

OSF_SAVE_LOAD_HOOK_EXPORT const OSFSaveLoadHookAPI* OSF_SAVE_LOAD_HOOK_CALL
    OSF_RequestSaveLoadHookAPI(uint32_t requestedVersion);

#ifdef __cplusplus
} // extern "C"
#endif
