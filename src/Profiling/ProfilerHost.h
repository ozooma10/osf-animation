#pragma once

// Engine side of the development-only profiler (docs/PROFILING.md): output directory, the
// command.json control channel, automatic dumps and [Profile] logging. Every macro below compiles to
// nothing unless OSF_PROFILING=1, and ProfilerHost.cpp is only built into the profiling override.

#if defined(OSF_PROFILING) && OSF_PROFILING

#include <string_view>

namespace OSF::Profiling::Host
{
	// Plugin load (main thread): starts a stats capture so boot and registry loading are included.
	void Initialize(std::string_view a_gameVersion);

	// Frame-maintenance thread, once per frame: reads <profiles>\command.json about once a second.
	void Poll() noexcept;

	// Queue one dump + reset for the next frame (coalesced) when automatic dumps are enabled.
	void OnSceneEnd(std::string_view a_sceneId) noexcept;
	void OnWorldReset(std::string_view a_reason) noexcept;
}

#define OSF_PROFILE_HOST_INIT(a_gameVersion) ::OSF::Profiling::Host::Initialize(a_gameVersion)
#define OSF_PROFILE_HOST_POLL() ::OSF::Profiling::Host::Poll()
#define OSF_PROFILE_HOST_SCENE_END(a_sceneId) ::OSF::Profiling::Host::OnSceneEnd(a_sceneId)
#define OSF_PROFILE_HOST_WORLD_RESET(a_reason) ::OSF::Profiling::Host::OnWorldReset(a_reason)

#else

#define OSF_PROFILE_HOST_INIT(a_gameVersion) ((void)0)
#define OSF_PROFILE_HOST_POLL() ((void)0)
#define OSF_PROFILE_HOST_SCENE_END(a_sceneId) ((void)0)
#define OSF_PROFILE_HOST_WORLD_RESET(a_reason) ((void)0)

#endif
