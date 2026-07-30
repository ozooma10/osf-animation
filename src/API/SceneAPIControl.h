#pragma once

namespace OSF::Scene
{
	struct SceneEvent;
}

// OSF-internal control surface for the native API (NOT shipped to consumers).
// The public, copyable header is API/OSFSceneAPI.h; this one only exposes the hooks main.cpp needs to bring the API online.
namespace OSF::API
{
	// Flip the native API's ready gate so the exported OSF_RequestSceneAPI factory returns the interface instead of nullptr.
	void MarkReady();

	// Synchronously deliver the runtime event to native API consumers before
	// SceneEventRelay queues the corresponding Papyrus calls.
	void DispatchNativeSceneEvent(const Scene::SceneEvent& a_event);
}
