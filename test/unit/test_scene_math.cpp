// Tests for PlacementToWorld: rotate a participant offset into the anchor's
// heading frame and add it to the anchor position. The single source of truth for
// both the initial teleport and the compose-root pin.
#include "framework/TestHarness.h"

#include "Animation/Scene.h"
#include "Util/Math.h"

using OSF::Animation::ParticipantPlacement;
using OSF::Animation::PlacementToWorld;

namespace
{
	constexpr float kPiF = OSF::Util::kPi;
}

OSF_TEST_CASE(PlacementToWorld_zero_offset_is_anchor)
{
	RE::NiPoint3 anchor{ 10.0f, 20.0f, 30.0f };
	auto p = PlacementToWorld(anchor, 1.234f, ParticipantPlacement{});
	CHECK_NEAR(p.x, 10.0f, 1e-4f);
	CHECK_NEAR(p.y, 20.0f, 1e-4f);
	CHECK_NEAR(p.z, 30.0f, 1e-4f);
}

OSF_TEST_CASE(PlacementToWorld_no_heading_is_translation)
{
	RE::NiPoint3 anchor{ 1.0f, 2.0f, 3.0f };
	ParticipantPlacement place{ 5.0f, 7.0f, 9.0f, 0.0f };
	auto p = PlacementToWorld(anchor, 0.0f, place);
	CHECK_NEAR(p.x, 6.0f, 1e-4f);
	CHECK_NEAR(p.y, 9.0f, 1e-4f);
	CHECK_NEAR(p.z, 12.0f, 1e-4f);
}

OSF_TEST_CASE(PlacementToWorld_rotates_offset_into_heading)
{
	RE::NiPoint3 anchor{ 0.0f, 0.0f, 0.0f };
	// A +1 on the local X axis, rotated by +90 degrees, lands on world +Y.
	ParticipantPlacement forward{ 1.0f, 0.0f, 0.0f, 0.0f };
	auto p = PlacementToWorld(anchor, kPiF / 2.0f, forward);
	CHECK_NEAR(p.x, 0.0f, 1e-4f);
	CHECK_NEAR(p.y, 1.0f, 1e-4f);
	CHECK_NEAR(p.z, 0.0f, 1e-4f);

	// Z is never rotated, only added.
	ParticipantPlacement up{ 0.0f, 0.0f, 4.0f, 0.0f };
	auto q = PlacementToWorld(anchor, kPiF / 2.0f, up);
	CHECK_NEAR(q.z, 4.0f, 1e-4f);
}
