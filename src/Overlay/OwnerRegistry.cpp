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

	OwnerRegistry::DispatchLease::~DispatchLease()
	{
		Reset();
	}

	OwnerRegistry::DispatchLease& OwnerRegistry::DispatchLease::operator=(DispatchLease&& a_rhs) noexcept
	{
		if (this == &a_rhs) return *this;
		Reset();
		_record = std::move(a_rhs._record);
		return *this;
	}

	void OwnerRegistry::DispatchLease::Reset() noexcept
	{
		if (!_record) return;
		auto record = std::move(_record);
		std::lock_guard l{ record->lock };
		--record->activeCallbacks;
		record->cv.notify_all();
	}

	API::OSFOverlayCallback OwnerRegistry::DispatchLease::Callback() const noexcept
	{
		return _record ? _record->callback : nullptr;
	}

	void* OwnerRegistry::DispatchLease::Context() const noexcept
	{
		return _record ? _record->context : nullptr;
	}

	std::string_view OwnerRegistry::DispatchLease::Plugin() const noexcept
	{
		return _record ? std::string_view(_record->plugin) : std::string_view{};
	}

	std::uint64_t OwnerRegistry::Acquire(std::string_view a_pluginId,
		API::OSFOverlayCallback a_callback, void* a_context)
	{
		if (a_pluginId.empty() || !a_callback) return 0;
		std::lock_guard l{ _lock };
		const auto plugin = OwnerKey(a_pluginId);
		if (_ownerByPlugin.contains(plugin)) return 0;
		std::uint64_t handle = _nextOwner++;
		if (handle == 0) handle = _nextOwner++;
		auto owner = std::make_shared<Record>();
		owner->plugin = plugin;
		owner->callback = a_callback;
		owner->context = a_context;
		_owners.emplace(handle, std::move(owner));
		_ownerByPlugin.emplace(plugin, handle);
		return handle;
	}

	bool OwnerRegistry::IsUsable(std::uint64_t a_owner) const
	{
		std::lock_guard l{ _lock };
		const auto found = _owners.find(a_owner);
		if (found == _owners.end()) return false;
		std::lock_guard ownerLock{ found->second->lock };
		return !found->second->releasing;
	}

	bool OwnerRegistry::MarkReleasing(std::uint64_t a_owner)
	{
		std::lock_guard l{ _lock };
		const auto found = _owners.find(a_owner);
		if (found == _owners.end()) return false;
		std::lock_guard ownerLock{ found->second->lock };
		found->second->releasing = true;
		return true;
	}

	bool OwnerRegistry::Release(std::uint64_t a_owner)
	{
		std::shared_ptr<Record> owner;
		{
			std::lock_guard l{ _lock };
			const auto found = _owners.find(a_owner);
			if (found == _owners.end()) return false;
			owner = found->second;
			{
				std::lock_guard ownerLock{ owner->lock };
				owner->releasing = true;
			}
			_ownerByPlugin.erase(owner->plugin);
			_owners.erase(found);
		}
		std::unique_lock ownerLock{ owner->lock };
		owner->cv.wait(ownerLock, [&]() { return owner->activeCallbacks == 0; });
		return true;
	}

	OwnerRegistry::DispatchLease OwnerRegistry::BeginDispatch(std::uint64_t a_owner)
	{
		std::lock_guard l{ _lock };
		const auto found = _owners.find(a_owner);
		if (found == _owners.end()) return {};
		auto owner = found->second;
		std::lock_guard ownerLock{ owner->lock };
		if (owner->releasing) return {};
		++owner->activeCallbacks;
		return DispatchLease(std::move(owner));
	}

	OwnerCallbackResult InvokeOwnerCallback(OwnerRegistry::DispatchLease a_lease,
		const API::OSFOverlayEvent& a_event, bool a_commit) noexcept
	{
		OwnerCallbackResult result;
		if (!a_lease) {
			result.acknowledged = !a_commit;
			return result;
		}
		result.delivered = true;
		result.plugin = a_lease.Plugin();
		try {
			result.acknowledged = a_lease.Callback()(&a_event, a_lease.Context());
		} catch (...) {
			result.threw = true;
			result.acknowledged = false;
		}
		if (!a_commit) result.acknowledged = true;
		return result;
	}
}
