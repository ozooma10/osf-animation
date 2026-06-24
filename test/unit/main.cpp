// Entry point for the offline osf-tests target. Sets the working directory to the
// fixtures root (so SceneRegistry::LoadAll() finds Data/OSF), loads the registry
// once, then runs every self-registered test case.
#include "framework/TestHarness.h"

#include "Registry/SceneRegistry.h"

#include <cstdio>
#include <filesystem>

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	// Fixtures dir: argv[1] override, else the compile-time path baked by xmake.
	std::filesystem::path fixtures =
#ifdef OSF_TEST_FIXTURES_DIR
		OSF_TEST_FIXTURES_DIR;
#else
		std::filesystem::current_path();
#endif
	if (argc > 1) {
		fixtures = argv[1];
	}

	std::error_code ec;
	std::filesystem::current_path(fixtures, ec);
	if (ec) {
		std::printf("FATAL: cannot chdir to fixtures '%s': %s\n",
			fixtures.string().c_str(), ec.message().c_str());
		return 2;
	}
	if (!std::filesystem::is_directory(fixtures / "Data" / "OSF")) {
		std::printf("FATAL: no Data/OSF under fixtures '%s'\n", fixtures.string().c_str());
		return 2;
	}

	OSF::Registry::SceneRegistry::GetSingleton().LoadAll();

	return osftest::RunAll();
}
