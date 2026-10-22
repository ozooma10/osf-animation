#pragma once

#include "API/OSFSceneAPI.h"
#include "Scene/SceneEventRelay.h"

namespace OSF::API
{
	// Borrowed strings remain valid for synchronous native dispatch only.
	inline OSFSceneEvent NativeEventPayload(const Scene::SceneEvent& event)
	{
		return {
			.size = sizeof(OSFSceneEvent), .sceneHandle = event.scene, .eventType = event.event,
			.node = event.node.c_str(), .edge = event.edge.c_str(), .cue = event.cue.c_str(),
			.actionType = event.actionType.c_str(), .actor = event.actor, .role = event.role.c_str(),
			.loopIndex = event.loopIndex, .time = event.time, .anchor = event.trackPosition.c_str(),
			.result = event.result, .cause = event.cause, .detectedNs = event.detectedNs
		};
	}
}
