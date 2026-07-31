#pragma once

#include "Util/KeywordLabel.h"

#include "RE/B/BGSKeyword.h"

namespace OSF::API
{
	inline std::string KeywordLabel(RE::BGSKeyword* a_keyword)
	{
		const char* editorId = a_keyword ? a_keyword->GetFormEditorID() : nullptr;
		return editorId ? Util::AnimationKeywordLabel(editorId) : std::string{};
	}
}
