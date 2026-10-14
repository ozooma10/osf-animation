#include "API/BrowserPlacement.h"
#include "API/EngineClient.h"
#include <cmath>

namespace OSF::API::BrowserPlacement
{
	bool InFrontOfView(RE::Actor* actor, float distance, OSFStartOptions& options)
	{
		if (!actor || !std::isfinite(distance)) return false;
		float base[3], heading;
		const auto& api = EngineClient::Get();
		if (!api.ReadWorldAnchor(actor, base, &heading)) return false;
		const auto camera = RE::PlayerCamera::GetSingleton();
		if (const auto root = camera ? camera->cameraRoot : nullptr) {
			const float x = root->world.rotate[1][0], y = root->world.rotate[1][1];
			const float length = x*x + y*y;
			if (std::isfinite(length) && length >= 0.0001f) heading = std::atan2(-x, y);
		}
		float position[3] = {base[0] - std::sin(heading)*distance, base[1] + std::cos(heading)*distance, base[2]};
		float snapped[3];
		if (api.SnapWorldPosition(actor, position, snapped)) {
			const float dx = snapped[0]-position[0], dy = snapped[1]-position[1], lateral = distance+1.0f;
			if (std::abs(snapped[2]-position[2]) <= 2.0f && dx*dx + dy*dy <= lateral*lateral)
				std::copy_n(snapped, 3, position);
		}
		options.anchorRef = nullptr; options.hasAnchor = true;
		options.anchorX = position[0]; options.anchorY = position[1]; options.anchorZ = position[2]; options.anchorHeadingRad = heading;
		return true;
	}
}
