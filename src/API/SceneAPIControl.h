#pragma once

namespace OSF::Scene
{
	struct SceneEvent;
}

// OSF-internal native API hooks; not shipped to consumers.
namespace OSF::API
{
	// Makes OSF_RequestSceneAPI return the interface instead of nullptr.
	void MarkReady();

	// Delivers synchronously to native consumers, before Papyrus events are queued.
	void DispatchNativeSceneEvent(const Scene::SceneEvent& a_event);
}
