#pragma once

#include "API/OSFOverlayAPI.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace OSF::Overlay
{
	class OwnerRegistry
	{
		struct Record;

	public:
		class DispatchLease
		{
		public:
			DispatchLease() = default;
			DispatchLease(DispatchLease&&) noexcept = default;
			DispatchLease& operator=(DispatchLease&& a_rhs) noexcept;
			DispatchLease(const DispatchLease&) = delete;
			DispatchLease& operator=(const DispatchLease&) = delete;
			~DispatchLease();

			[[nodiscard]] explicit operator bool() const noexcept { return _record != nullptr; }
			[[nodiscard]] API::OSFOverlayCallback Callback() const noexcept;
			[[nodiscard]] void* Context() const noexcept;
			[[nodiscard]] std::string_view Plugin() const noexcept;

		private:
			friend class OwnerRegistry;
			explicit DispatchLease(std::shared_ptr<Record> a_record) : _record(std::move(a_record)) {}
			void Reset() noexcept;
			std::shared_ptr<Record> _record;
		};

		std::uint64_t Acquire(std::string_view a_pluginId, API::OSFOverlayCallback a_callback, void* a_context);
		bool IsUsable(std::uint64_t a_owner) const;
		bool MarkReleasing(std::uint64_t a_owner);
		bool Release(std::uint64_t a_owner);
		DispatchLease BeginDispatch(std::uint64_t a_owner);

	private:
		struct Record
		{
			std::string plugin;
			API::OSFOverlayCallback callback = nullptr;
			void* context = nullptr;
			std::mutex lock;
			std::condition_variable cv;
			std::size_t activeCallbacks = 0;
			bool releasing = false;
		};

		mutable std::mutex _lock;
		std::unordered_map<std::uint64_t, std::shared_ptr<Record>> _owners;
		std::unordered_map<std::string, std::uint64_t> _ownerByPlugin;
		std::uint64_t _nextOwner = 1;
	};

	struct OwnerCallbackResult
	{
		bool delivered = false;
		bool acknowledged = false;
		bool threw = false;
		std::string plugin;
	};

	OwnerCallbackResult InvokeOwnerCallback(OwnerRegistry::DispatchLease a_lease,
		const API::OSFOverlayEvent& a_event, bool a_commit) noexcept;
}
