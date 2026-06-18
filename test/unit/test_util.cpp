// Tests for the header-only utilities: case folding and angle wrapping.
#include "framework/TestHarness.h"

#include "Util/Math.h"
#include "Util/StringUtil.h"

using OSF::Util::ToLower;
using OSF::Util::WrapRadians;
using OSF::Util::kPi;

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

OSF_TEST_CASE(WrapRadians_passes_in_range)
{
	CHECK_NEAR(WrapRadians(0.0f), 0.0f, 1e-5f);
	CHECK_NEAR(WrapRadians(1.0f), 1.0f, 1e-5f);
	CHECK_NEAR(WrapRadians(-1.0f), -1.0f, 1e-5f);
	// pi stays (the range is (-pi, pi]).
	CHECK_NEAR(WrapRadians(kPi), kPi, 1e-5f);
}

OSF_TEST_CASE(WrapRadians_wraps_out_of_range)
{
	// Just over pi wraps to just over -pi.
	CHECK_NEAR(WrapRadians(kPi + 0.5f), -kPi + 0.5f, 1e-4f);
	// Just under -pi wraps to just under pi.
	CHECK_NEAR(WrapRadians(-kPi - 0.5f), kPi - 0.5f, 1e-4f);
	// Multiple turns collapse to the principal value.
	CHECK_NEAR(WrapRadians(4.0f * kPi + 0.3f), 0.3f, 1e-4f);
	CHECK_NEAR(WrapRadians(-4.0f * kPi - 0.3f), -0.3f, 1e-4f);
}
