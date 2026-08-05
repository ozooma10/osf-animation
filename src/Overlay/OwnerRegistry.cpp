#include "Overlay/OwnerRegistry.h"

#include <algorithm>
#include <cctype>

namespace OSF::Overlay
{
	namespace
	{
		std::string OwnerKey(std::string_view a_pluginId)
		{
			std::string key(a_pluginId);
			std::transform(key.begin(), key.end(), key.begin(), [](unsigned char a_ch) {
				return static_cast<char>(std::tolower(a_ch));
			});
			return key;
		}
	}

	std::uint64_t OwnerRegistry::Acquire(std::string_view a_pluginId,
		API::OSFOverlayCallback a_callback, void* a_context)
	{
		if (a_pluginId.empty() || !a_callback) return 0;
		const auto plugin = OwnerKey(a_pluginId);
		if (_ownerByPlugin.contains(plugin)) return 0;
		std::uint64_t handle = _nextOwner++;
		if (handle == 0) handle = _nextOwner++;
		_owners.emplace(handle, Record{ plugin, a_callback, a_context });
		_ownerByPlugin.emplace(plugin, handle);
		return handle;
	}

	bool OwnerRegistry::IsUsable(std::uint64_t a_owner) const
	{
		return _owners.contains(a_owner);
	}

	bool OwnerRegistry::Release(std::uint64_t a_owner)
	{
		const auto found = _owners.find(a_owner);
		if (found == _owners.end()) return false;
		_ownerByPlugin.erase(found->second.plugin);
		_owners.erase(found);
		return true;
	}

	OwnerCallback OwnerRegistry::GetCallback(std::uint64_t a_owner) const
	{
		const auto found = _owners.find(a_owner);
		return found == _owners.end() ? OwnerCallback{} :
			OwnerCallback{ found->second.callback, found->second.context, found->second.plugin };
	}

	OwnerCallbackResult InvokeOwnerCallback(const OwnerCallback& a_target,
		const API::OSFOverlayEvent& a_event, bool a_commit) noexcept
	{
		OwnerCallbackResult result;
		if (!a_target) {
			result.acknowledged = !a_commit;
			return result;
		}
		result.plugin = a_target.plugin;
		try {
			result.acknowledged = a_target.callback(&a_event, a_target.context);
		} catch (...) {
			result.threw = true;
			result.acknowledged = false;
		}
		if (!a_commit) result.acknowledged = true;
		return result;
	}
}
