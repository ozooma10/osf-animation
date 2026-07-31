#include "Animation/FrameClock.h"
#include "Util/ClipPath.h"
#include "Util/DiagnosticText.h"
#include "Util/KeywordLabel.h"
#include "Util/RegistryFiles.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
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

	{
		const auto data = (std::filesystem::current_path() / "Data").lexically_normal();
		const auto rel = OSF::Util::DataRelativePath(data / "..extras" / "clip.af");
		Check(rel && *rel == OSF::Util::NormalizeResourcePath("..extras/clip.af"),
			"Data child beginning with two dots is not traversal");
	}
	{
		const auto data = (std::filesystem::current_path() / "Data").lexically_normal();
		std::string differentlyCased = data.string();
		std::transform(differentlyCased.begin(), differentlyCased.end(), differentlyCased.begin(),
			[](unsigned char a_ch) { return static_cast<char>(std::tolower(a_ch)); });
		const auto rel = OSF::Util::DataRelativePath(
			std::filesystem::path{ differentlyCased } / "OSF" / "clip.af");
		Check(rel && *rel == OSF::Util::NormalizeResourcePath("OSF/clip.af"),
			"Data containment follows Windows case-insensitivity");
	}
	{
		const auto outside = std::filesystem::current_path() / "Elsewhere" / "clip.af";
		Check(!OSF::Util::DataRelativePath(outside), "path outside Data is not archive-relative");
	}
	{
		const std::filesystem::path root{ R"(C:\Game\Data\OSF)" };
		const auto label = OSF::Util::RegistryPathLabel(root, root / "Packs" / "test.osf.json");
		Check(label == "Packs/test.osf.json", "registry labels are root-relative");
	}
	{
		const auto safe = OSF::Util::SanitizeDiagnosticPaths(
			R"(cannot inspect 'C:\Users\Player\Game\Data\OSF\bad.osf.json': denied)");
		Check(safe.find("C:\\Users") == std::string::npos, "health context removes absolute paths");
		Check(safe.find("'bad.osf.json'") != std::string::npos, "health context keeps the filename");
	}
	{
		Check(OSF::Util::AnimationKeywordLabel("AnimFurnChairScrappy") == "Chair Scrappy",
			"animation keyword prefix and CamelCase are humanized");
		Check(OSF::Util::AnimationKeywordLabel("AnimHVACUnit") == "HVAC Unit",
			"animation keyword acronym boundary is preserved");
	}

	std::cout << "Core runtime tests passed\n";
	return 0;
}
