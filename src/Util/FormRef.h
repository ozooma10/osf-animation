#pragma once

#include "Util/StringUtil.h"

#include <charconv>
#include <optional>
#include <string>
#include <utility>

namespace OSF::Util
{
	struct ParsedFormRef
	{
		std::string plugin;
		std::uint32_t local;
	};

	// Syntax only: does not require the plugin or the game to be loaded.
	inline std::optional<ParsedFormRef> ParseFormRef(const std::string& a_ref)
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
			return std::nullopt;
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
				return std::nullopt;
			}
		}
		return ParsedFormRef{ std::move(plugin), local };
	}

	// "<plugin>|0x<localId>" -> runtime FormID. The local id's load-order bits are ignored, so a
	// FormID pasted whole from xEdit resolves.
	inline std::optional<RE::TESFormID> ComposeFormID(const std::string& a_ref)
	{
		const auto parsed = ParseFormRef(a_ref);
		if (!parsed) return std::nullopt;
		const auto local = parsed->local;
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return std::nullopt;
		}
		const std::string_view want = parsed->plugin;
		auto nameMatches = [&](RE::TESFile* a_file) {
			return a_file && IEquals(a_file->fileName, want);
		};
		const auto& c = dh->compiledFileCollection;
		for (std::uint32_t i = 0; i < c.files.size(); i++) {
			if (nameMatches(c.files[i])) {
				return (i << 24) | (local & 0x00FFFFFFu);
			}
		}
		for (std::uint32_t i = 0; i < c.mediumFiles.size(); i++) {
			if (nameMatches(c.mediumFiles[i])) {
				return 0xFD000000u | (i << 16) | (local & 0x0000FFFFu);
			}
		}
		for (std::uint32_t i = 0; i < c.smallFiles.size(); i++) {
			if (nameMatches(c.smallFiles[i])) {
				return 0xFE000000u | (i << 12) | (local & 0x00000FFFu);
			}
		}
		return std::nullopt;
	}

	// T must be a concrete form type: LookupByID<T> tests exact FormType, so an abstract base like
	// TESBoundObject never matches. Use ResolveBoundObject for those.
	template <class T>
	T* ResolveFormRef(const std::string& a_ref)
	{
		const auto id = ComposeFormID(a_ref);
		return id ? RE::TESForm::LookupByID<T>(*id) : nullptr;
	}

	// Form types safe to static_cast to TESBoundObject* (base at offset 0).
	inline bool IsBoundObjectType(RE::FormType a_type)
	{
		using FT = RE::FormType;
		switch (a_type) {
		case FT::kACTI:
		case FT::kTACT:
		case FT::kARMO:
		case FT::kBOOK:
		case FT::kCONT:
		case FT::kDOOR:
		case FT::kINGR:
		case FT::kLIGH:
		case FT::kMISC:
		case FT::kSTAT:
		case FT::kSCOL:
		case FT::kPKIN:
		case FT::kMSTT:
		case FT::kGRAS:
		case FT::kFLOR:
		case FT::kFURN:
		case FT::kWEAP:
		case FT::kAMMO:
		case FT::kKEYM:
		case FT::kALCH:
		case FT::kIDLM:
		case FT::kNOTE:
		case FT::kPROJ:
		case FT::kHAZD:
		case FT::kBNDS:
		case FT::kSLGM:
		case FT::kTERM:
			return true;
		default:
			return false;
		}
	}

	inline RE::TESBoundObject* ResolveBoundObject(const std::string& a_ref)
	{
		const auto id = ComposeFormID(a_ref);
		if (!id) {
			return nullptr;
		}
		auto* form = RE::TESForm::LookupByID(*id);
		return (form && IsBoundObjectType(form->GetFormType())) ? static_cast<RE::TESBoundObject*>(form) : nullptr;
	}
}
