#include "Animation/FrameClock.h"
#include "Util/ClipPath.h"

#include <cstdlib>
#include <iostream>

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
	OSF::Animation::FrameClock clock;
	int ownerToken = 0;
	int survivorToken = 0;

	Check(clock.ShouldAdvance(&ownerToken, 1000), "first reporter becomes owner");
	Check(!clock.ShouldAdvance(&survivorToken, 1100), "owner is retained inside stale window");
	Check(clock.ShouldAdvance(&survivorToken, 1300), "live reporter replaces stale owner");
	Check(clock.owner == &survivorToken, "replacement owner is recorded");

	clock.time = 42.0f;
	clock.Reset();
	Check(clock.owner == nullptr, "reset clears owner");
	Check(clock.time == 0.0f, "reset clears time");
	Check(clock.lastAdvanceMs == 0, "reset clears owner heartbeat");

	{
		auto [path, animation] = OSF::Util::SplitRuntimeClipSpec("Data/OSF/test.glb:idle");
		Check(path == "Data/OSF/test.glb", "glTF selector keeps the path");
		Check(animation == "idle", "glTF selector extracts the animation id");
	}
	{
		auto [path, animation] = OSF::Util::SplitRuntimeClipSpec("C:\\clips\\test.af");
		Check(path == "C:\\clips\\test.af", "non-glTF colon remains part of the path");
		Check(animation.empty(), "non-glTF path has no selector");
	}

	std::cout << "Core runtime tests passed\n";
	return 0;
}
