#pragma once

#include "API/OSFOverlayAPI.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace OSF::Overlay
{
	struct OwnerCallback
	{
		API::OSFOverlayCallback callback = nullptr;
		void* context = nullptr;
		std::string plugin;

		[[nodiscard]] explicit operator bool() const noexcept { return callback != nullptr; }
	};

	// OverlayService serializes every access. Keeping callback lifetime and service state behind one
	// lock avoids a second mutex/condition-variable ownership protocol.
	class OwnerRegistry
	{
	public:
		std::uint64_t Acquire(std::string_view a_pluginId, API::OSFOverlayCallback a_callback, void* a_context);
		bool IsUsable(std::uint64_t a_owner) const;
		bool Release(std::uint64_t a_owner);
		OwnerCallback GetCallback(std::uint64_t a_owner) const;

	private:
		struct Record
		{
			std::string plugin;
			API::OSFOverlayCallback callback = nullptr;
			void* context = nullptr;
		};

		std::unordered_map<std::uint64_t, Record> _owners;
		std::unordered_map<std::string, std::uint64_t> _ownerByPlugin;
		std::uint64_t _nextOwner = 1;
	};

	struct OwnerCallbackResult
	{
		bool acknowledged = false;
		bool threw = false;
		std::string plugin;
	};

	OwnerCallbackResult InvokeOwnerCallback(const OwnerCallback& a_target,
		const API::OSFOverlayEvent& a_event, bool a_commit) noexcept;
}
