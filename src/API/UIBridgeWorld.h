#pragma once

#include <cstdint>
#include <string>

namespace RE
{
	class BGSKeyword;
	class TESObjectREFR;
}

namespace OSFUI::API
{
	class Client;
}

namespace OSF::API::UIBridgeWorld
{
	// Internal implementation boundary; UIBridge.h remains the public surface.
	// Browser-session references live with the world-facing bridge handlers. All
	// access is on the game main thread; -1 remains the reserved player token.
	std::int32_t AllocToken(RE::TESObjectREFR* a_ref);
	RE::TESObjectREFR* ResolveToken(std::int32_t a_token);
	std::string ScanLabel(RE::TESObjectREFR* a_ref, RE::BGSKeyword* a_matchedKw = nullptr);
	std::string RefSexTag(RE::TESObjectREFR* a_ref);
	RE::TESObjectREFR* CrosshairRef();
	void ClearSessionTokens();

	// Registers the world-facing command group in its established order.
	void RegisterCommands(OSFUI::API::Client& a_ui);
}
