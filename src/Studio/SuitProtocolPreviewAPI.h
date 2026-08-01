#pragma once

#include "REX/W32/KERNEL32.h"

#include <cstddef>
#include <cstdint>

namespace OSF::Studio::Suit
{
	constexpr std::uint32_t kAPIVersion = 1;
	constexpr auto kModuleName = L"Suit Protocol.dll";
	constexpr auto kExportName = "SuitProtocol_RequestStudioHelmetPreviewAPI";

	enum class HelmetState : std::uint32_t
	{
		kEquipped = 0,
		kHeld = 1,
		kStowed = 2
	};

	struct Attachment
	{
		char  node[64]{};
		float position[3]{};
		float rotation[3]{};
		float scale{ 1.0F };
	};

	struct Setup
	{
		std::uint32_t size{ sizeof(Setup) };
		Attachment    stowed;
		Attachment    handoff;
		std::uint32_t handoffHoldMs{};
		std::uint32_t handoffSettleMs{};
		std::uint32_t leaseMs{ 30000 };
	};

	struct API
	{
		std::uint32_t version;
		bool (*Begin)(const Setup*, char*, std::size_t);
		bool (*ApplyState)(HelmetState, char*, std::size_t);
		bool (*Renew)(std::uint32_t, char*, std::size_t);
		void (*Stop)();
	};

	inline const API* Acquire()
	{
		const auto module = REX::W32::GetModuleHandleW(kModuleName);
		if (!module) return nullptr;
		using Request = const API* (*)(std::uint32_t);
		const auto request = reinterpret_cast<Request>(
			REX::W32::GetProcAddress(module, kExportName));
		const auto* api = request ? request(kAPIVersion) : nullptr;
		return api && api->version == kAPIVersion && api->Begin && api->ApplyState &&
			api->Renew && api->Stop ? api : nullptr;
	}
}
