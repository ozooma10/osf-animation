#pragma once

// Shared "<Plugin>.esm|0xLOCAL" form-ref resolution. Handles the FormID composition used by the scene registry's role filters 

#include "Util/StringUtil.h"

#include <charconv>
#include <optional>
#include <string>

namespace OSF::Util
{
	// Compose a runtime FormID from a "<plugin>|0x<localId>" ref, or nullopt if malformed or the plugin isn't loaded. 
	// The plugin name is matched case-insensitively by basename; 
	// the local id's high/load-order bits are IGNORED (the plugin name is authoritative), so a FormID pasted whole from xEdit resolves regardless of its load-order byte. 
	// Light/medium tiers derive their secondary index from the file's position in smallFiles/mediumFiles.
	inline std::optional<RE::TESFormID> ComposeFormID(const std::string& a_ref)
	{
		const auto bar = a_ref.find('|');
		if (bar == std::string::npos) {
			return std::nullopt;
		}
		std::string plugin = a_ref.substr(0, bar);
		std::string idStr = a_ref.substr(bar + 1);
		if (plugin.empty() || idStr.empty()) {
			return std::nullopt;
		}
		if (plugin.find('/') != std::string::npos || plugin.find('\\') != std::string::npos) {
			return std::nullopt;  // a path in the plugin name is malformed
		}
		std::uint32_t local = 0;
		{
			const char* b = idStr.data();
			const char* e = b + idStr.size();
			if (idStr.size() > 2 && (idStr[0] == '0') && (idStr[1] == 'x' || idStr[1] == 'X')) {
				b += 2;
			}
			const auto [ptr, ec] = std::from_chars(b, e, local, 16);
			if (ec != std::errc{} || ptr != e) {
				return std::nullopt;  // not a hex local id
			}
		}
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return std::nullopt;
		}
		const auto want = ToLower(plugin);
		auto nameMatches = [&](RE::TESFile* a_file) {
			return a_file && ToLower(a_file->fileName) == want;
		};
		const auto& c = dh->compiledFileCollection;
		for (std::uint32_t i = 0; i < c.files.size(); i++) {
			if (nameMatches(c.files[i])) {
				return (i << 24) | (local & 0x00FFFFFFu);  // full master
			}
		}
		for (std::uint32_t i = 0; i < c.mediumFiles.size(); i++) {
			if (nameMatches(c.mediumFiles[i])) {
				return 0xFD000000u | (i << 16) | (local & 0x0000FFFFu);  // medium (.esm medium tier)
			}
		}
		for (std::uint32_t i = 0; i < c.smallFiles.size(); i++) {
			if (nameMatches(c.smallFiles[i])) {
				return 0xFE000000u | (i << 12) | (local & 0x00000FFFu);  // light (.esl/ESL-flagged)
			}
		}
		return std::nullopt;  // plugin not loaded
	}

	// Resolve a form ref to T*, or nullptr (malformed / unloaded plugin / not-found / wrong type).
	// LookupByID<T> already returns null for not-found OR wrong-type. Callers that need a precise error message should resolve via ComposeFormID + LookupByID themselves.
	template <class T>
	T* ResolveFormRef(const std::string& a_ref)
	{
		const auto id = ComposeFormID(a_ref);
		return id ? RE::TESForm::LookupByID<T>(*id) : nullptr;
	}
}
