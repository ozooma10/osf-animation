// Tests for SoundRegistry clip text — the "voice line = sound clip + subtitle" path. A clip may carry
// spoken text (object-form `clips`, or `text` on an array entry); TextForClip(spec) returns it so a
// played clip renders its line. Fixture: test/fixtures/Data/OSF/sounds_text.sounds.json.
#include "framework/TestHarness.h"

#include "Registry/SoundRegistry.h"

#include <string>

using OSF::Registry::SoundRegistry;

OSF_TEST_CASE(Sound_clip_text_object_and_array_forms)
{
	auto& reg = SoundRegistry::GetSingleton();
	reg.LoadAll();  // scans the fixtures Data/OSF for *.sounds.json (own singleton; isolated from scenes)

	// Object form: clip path -> text.
	CHECK_EQ(reg.TextForClip("Sound/OSF/Test/a.wav"), std::string("Line A spoken."));
	// Object value "" -> no subtitle (clip plays silently).
	CHECK_EQ(reg.TextForClip("Sound/OSF/Test/silent.wav"), std::string());
	// Array form with `text`.
	CHECK_EQ(reg.TextForClip("Sound/OSF/Test/b.wav"), std::string("Line B spoken."));
	// Array entry without `text`, and an unknown clip -> "".
	CHECK_EQ(reg.TextForClip("Sound/OSF/Test/c.wav"), std::string());
	CHECK_EQ(reg.TextForClip("Sound/OSF/Test/nope.wav"), std::string());

	// Both forms still load as real, resolvable pools.
	CHECK(reg.Pick({ "test", "objform" }).has_value());
	CHECK(reg.Pick({ "test", "arrform" }).has_value());
}
