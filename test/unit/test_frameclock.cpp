// Tests for the owner-token FrameClock: only the first-seen manager advances the
// clock, so subdivided / multi-manager update reports don't over-advance time.
#include "framework/TestHarness.h"

#include "Animation/FrameClock.h"

using OSF::Animation::FrameClock;
using OSF::Animation::SyncGroup;

OSF_TEST_CASE(FrameClock_first_reporter_becomes_owner)
{
	FrameClock clock;
	int token_a = 0;
	int token_b = 0;

	CHECK(clock.owner == nullptr);
	CHECK(clock.ShouldAdvance(&token_a));   // first reporter wins ownership
	CHECK(clock.owner == &token_a);
	CHECK(clock.ShouldAdvance(&token_a));   // owner keeps advancing
	CHECK(!clock.ShouldAdvance(&token_b));  // a non-owner never advances
}

OSF_TEST_CASE(FrameClock_reset_clears_owner_and_time)
{
	FrameClock clock;
	int token_a = 0;
	int token_b = 0;

	clock.ShouldAdvance(&token_a);
	clock.time = 5.0f;
	clock.Reset();

	CHECK(clock.owner == nullptr);
	CHECK_NEAR(clock.time, 0.0f, 1e-6f);
	// After reset a different manager can claim ownership.
	CHECK(clock.ShouldAdvance(&token_b));
	CHECK(clock.owner == &token_b);
}

OSF_TEST_CASE(SyncGroup_defaults)
{
	SyncGroup group;
	CHECK(group.clock.owner == nullptr);
	CHECK_EQ(group.lastAdvanceMs, static_cast<int64_t>(0));
	CHECK_NEAR(group.speed.load(), 1.0f, 1e-6f);
}
