// Tests for the header-only utilities: case folding.
#include "framework/TestHarness.h"

#include "Util/StringUtil.h"

using OSF::Util::ToLower;

OSF_TEST_CASE(ToLower_folds_ascii)
{
	CHECK_EQ(ToLower("HELLO"), std::string("hello"));
	CHECK_EQ(ToLower("MixedCase123"), std::string("mixedcase123"));
	CHECK_EQ(ToLower("already.lower"), std::string("already.lower"));
}

OSF_TEST_CASE(ToLower_empty_and_symbols)
{
	CHECK_EQ(ToLower(""), std::string(""));
	// Non-alphabetic bytes are left untouched (path separators, hex, punctuation).
	CHECK_EQ(ToLower("OSF\\Anim_01.GLB"), std::string("osf\\anim_01.glb"));
	CHECK_EQ(ToLower("Plugin.ESM|0xABCDEF"), std::string("plugin.esm|0xabcdef"));
}
