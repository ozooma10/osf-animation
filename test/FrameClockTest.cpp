#include "Check.h"

#include "Animation/FrameClock.h"
#include "Animation/StallWatchdogSchedule.h"
#include "Util/ClipPath.h"
#include "Util/DiagnosticText.h"
#include "Util/NativeMainThreadQueue.h"
#include "Util/RegistryFiles.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <thread>

using OSF::Test::Check;
using OSF::Test::Finish;

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

	OSF::Animation::StallWatchdogSchedule watchdog;
	Check(!watchdog.ShouldScan(1000), "watchdog first beat arms resume grace");
	for (std::int64_t now = 1200; now < 3000; now += 200) {
		Check(!watchdog.ShouldScan(now), "watchdog does not scan inside resume grace");
	}
	Check(!watchdog.ShouldScan(2999), "watchdog does not scan inside resume grace");
	Check(watchdog.ShouldScan(3000), "watchdog scans when resume grace expires");
	Check(!watchdog.ShouldScan(3100), "watchdog scan interval throttles frame beats");
	Check(watchdog.ShouldScan(3250), "watchdog scans again after its interval");
	watchdog.Pause();
	Check(!watchdog.ShouldScan(10000), "watchdog pause forces fresh resume grace");
	for (std::int64_t now = 10200; now < 12000; now += 200) {
		Check(!watchdog.ShouldScan(now), "watchdog keeps pause grace armed across frame beats");
	}
	Check(watchdog.ShouldScan(12000), "watchdog resumes scanning after pause grace");
	Check(!watchdog.ShouldScan(12600), "watchdog treats a long frame gap as another resume");

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
		namespace Queue = OSF::Util::NativeMainThreadQueue;
		std::vector<int> events;
		Queue::PostBatch({
			[&] {
				events.push_back(1);  // final timed mark
				// A producer can publish during a callback without waiting on the drain's lock.
				std::thread producer([&] { Queue::Post([&] { events.push_back(3); }); });
				producer.join();
				Queue::Post([&] { events.push_back(4); });
			},
			[&] { events.push_back(2); }  // auto-end follows final marks
		});
		Check(events.empty(), "frame commands never execute on the posting thread");
		Queue::Drain();
		Check(events == std::vector<int>{ 1, 2 }, "drain completes its captured batch before callback-enqueued work");
		Queue::Drain();
		Check(events == std::vector<int>{ 1, 2, 3, 4 }, "worker and nested posts wait for the next frame in FIFO order");
		Queue::Drain();
		Check(events.size() == 4, "commands execute once");
	}
	{
		namespace Queue = OSF::Util::NativeMainThreadQueue;
		constexpr int producerCount = 4;
		constexpr int commandsPerProducer = 64;
		std::array<int, producerCount> next{};
		bool ordered = true;
		bool onOwnerThread = true;
		const auto owner = std::this_thread::get_id();
		std::vector<std::thread> producers;
		for (int producer = 0; producer < producerCount; ++producer) {
			producers.emplace_back([&, producer] {
				for (int command = 0; command < commandsPerProducer; ++command) {
					Queue::Post([&, producer, command] {
						onOwnerThread &= std::this_thread::get_id() == owner;
						ordered &= next[producer]++ == command;
					});
				}
			});
		}
		for (auto& producer : producers) producer.join();
		Queue::Drain();
		Check(onOwnerThread && ordered, "concurrent producers retain their order and execute on the drain owner");
		Check(std::ranges::all_of(next, [](int count) { return count == commandsPerProducer; }),
			"concurrent posts are neither lost nor duplicated");
	}

	return Finish("Core runtime");
}
