#include "Registry/SoundRegistry.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
	void Check(bool a_condition, const char* a_message)
	{
		if (!a_condition) {
			std::cerr << "FAIL: " << a_message << '\n';
			std::exit(1);
		}
	}
}

int main()
{
	// xmake runs this target with test/fixtures as cwd, so LoadAll sees Data/OSF.
	auto& reg = OSF::Registry::SoundRegistry::GetSingleton();
	reg.LoadAll();
	const auto files = reg.FileStats();
	bool sawFixture = false;
	for (const auto& file : files) {
		if (file.file == "sounds_text.sounds.json") {
			sawFixture = true;
			Check(file.errors == 0 && file.warnings == 0 && file.problems.empty(), "sound file stats: clean fixture");
			Check(file.path == "sounds_text.sounds.json", "sound file stats: Data/OSF-relative path");
		}
	}
	Check(sawFixture, "sound file stats: fixture present");


	Check(reg.TextForClip("Sound/OSF/Test/a.wav") == "Line A spoken.", "object-form subtitle");
	Check(reg.TextForClip("Sound/OSF/Test/silent.wav").empty(), "empty object-form subtitle");
	Check(reg.TextForClip("Sound/OSF/Test/b.wav") == "Line B spoken.", "array-form subtitle");
	Check(reg.TextForClip("Sound/OSF/Test/c.wav").empty(), "array clip without subtitle");
	Check(reg.TextForClip("Sound/OSF/Test/nope.wav").empty(), "unknown clip subtitle");
	Check(reg.Pick({ "test", "objform" }).has_value(), "object-form pool resolves");
	Check(reg.Pick({ "test", "arrform" }).has_value(), "array-form pool resolves");

	std::cout << "Sound registry tests passed\n";
	return 0;
}
