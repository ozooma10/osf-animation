#pragma once
#include "API/CatalogBuilder.h"
#include "API/CatalogJson.h"
#include "API/OSFEngineAPI.h"
#include "API/OSFClipScene.h"
#include "Profiling/Profile.h"
#include <cstring>
#include <optional>

namespace OSF::API::ClipProviderBridge
{
	// Called synchronously under the registry's provider lock, against one pinned base snapshot.
	inline std::vector<ClipScene> Invoke(const Registry::ContentRegistrySnapshot& source,
		ClipSceneProvider provider, void* context, bool clipSources)
	{
		OSF_PROFILE_SCOPE("API.ClipProviderRebuild");
		using json = nlohmann::json;
		struct Output { std::uint32_t calls{}; std::optional<std::vector<ClipScene>> scenes; } output;
		const auto text = [&] {
			OSF_PROFILE_SCOPE("API.ClipProviderInput");
			if (!clipSources) return CatalogJson::Serialize(CatalogBuilder::Build(source, {}));
			return json(CatalogBuilder::BuildClipSources(source)).dump(-1, ' ', false, json::error_handler_t::replace);
		}();
		OSF_PROFILE_GAUGE("API.ClipProviderInput.Bytes", text.size());
		{
			OSF_PROFILE_SCOPE("API.ClipProviderInvoke");
			provider(text.c_str(), [](const char* response, void* value) noexcept {
				OSF_PROFILE_SCOPE("API.ClipProviderDecode");
				auto& out = *static_cast<Output*>(value);
				++out.calls;
				try {
					if (response && std::strlen(response) <= 16 * 1024 * 1024)
						out.scenes = json::parse(response).get<std::vector<ClipScene>>();
				} catch (...) { out.scenes.reset(); }
			}, &output, context);
		}
		if (output.calls != 1 || !output.scenes) throw std::runtime_error("invalid provider response");
		return std::move(*output.scenes);
	}
}
