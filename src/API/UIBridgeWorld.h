#pragma once

#include <cstdint>
#include <string>

namespace RE
{
	class BGSKeyword;
	class TESObjectREFR;
}

namespace OSF::API::UIBridgeWorld
{
	// Game main thread only; token -1 is reserved for the player.
	std::int32_t AllocToken(RE::TESObjectREFR* a_ref);
	RE::TESObjectREFR* ResolveToken(std::int32_t a_token);
	std::string ScanLabel(RE::TESObjectREFR* a_ref, RE::BGSKeyword* a_matchedKw = nullptr);
	std::string RefSexTag(RE::TESObjectREFR* a_ref);
	RE::TESObjectREFR* CrosshairRef();
	void ClearSessionTokens();

	void RegisterCommands();
}
