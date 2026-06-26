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
	// WARNING: T must be a CONCRETE leaf form type (one with its own FORMTYPE, e.g. BGSKeyword/TESRace).
	// Do NOT pass an abstract base like RE::TESBoundObject — LookupByID<T>/As<T> test EXACT FormType
	// equality (GetFormType() == T::FORMTYPE), and an abstract base has no concrete FormType of its own,
	// so the cast NEVER matches and the call always returns null. For bound objects use ResolveBoundObject.
	template <class T>
	T* ResolveFormRef(const std::string& a_ref)
	{
		const auto id = ComposeFormID(a_ref);
		return id ? RE::TESForm::LookupByID<T>(*id) : nullptr;
	}

	// True if a_type is a TESBoundObject-derived form type, i.e. a TESForm* of this type may be
	// safely static_cast to TESBoundObject* (which lives at offset 0). TESBoundObject is an abstract
	// base spanning many form types, so there is no single-FormType test for it — this is the set.
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

	// Resolve a "<plugin>|0xLOCAL" ref to a TESBoundObject* (equippable / inventory / placeable item),
	// or nullptr. Looks up the raw form then verifies it is a bound-object type before the cast — the
	// only correct way, since LookupByID<TESBoundObject> can never succeed (see ResolveFormRef warning).
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
