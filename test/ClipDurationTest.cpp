#include "Check.h"
#include "Serialization/ClipDurations.h"
#include "Serialization/ClipDurationCache.h"
#include "Serialization/ClipDurationProbe.h"
#include "API/EngineClient.h"
#include "Util/Ba2.h"
#include "Util/NativeMainThreadQueue.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <future>
#include <thread>

using OSF::Test::Check;
using namespace std::chrono_literals;
namespace Durations = OSF::Serialization::ClipDurations;
namespace Cache = OSF::Browser::ClipDurationCache;
using json = nlohmann::json;

namespace
{
	struct Gate
	{
		std::mutex mutex;
		std::condition_variable changed;
		bool armed = false, entered = false, released = false;
		void Arm() { std::lock_guard lock(mutex); armed = true; entered = released = false; }
		void Block()
		{
			std::unique_lock lock(mutex);
			if (!armed) return;
			entered = true;
			changed.notify_all();
			changed.wait(lock, [&] { return released; });
			armed = false;
		}
		bool Wait()
		{
			std::unique_lock lock(mutex);
			return changed.wait_for(lock, 5s, [&] { return entered; });
		}
		void Release() { std::lock_guard lock(mutex); released = true; changed.notify_all(); }
	} diskGate, probeGate, snapshotGate;
	std::atomic_bool delaySnapshot{};

	std::filesystem::path root;
	std::atomic<int> diskCalls{}, archiveCalls{};
	std::vector<OSF::Catalog::Clip> clips;
	std::mutex queueMutex;
	std::condition_variable queued;
	std::vector<std::function<void()>> commands;
	std::vector<std::uint8_t> archiveBytes(64);
	int notifications = 0;
	std::thread::id callbackThread;

	void OnChanged() { ++notifications; callbackThread = std::this_thread::get_id(); }
	void WaitScan()
	{
		std::vector<std::function<void()>> batch;
		{
			std::unique_lock lock(queueMutex);
			if (!queued.wait_for(lock, 10s, [] { return !commands.empty(); })) {
				// A detached worker must not outlive the test's fixtures after a failed rendezvous.
				Check(false, "duration scan completed and queued a catalog change");
				std::exit(1);
			}
			batch.swap(commands);
		}
		for (auto& command : batch) command();
	}

	json ReadCache()
	{
		std::ifstream input(root / "OSF/clip-durations.json");
		return json::parse(input);
	}
	void WriteGltf(const char* a_name, float a_seconds)
	{
		std::ofstream(root / "Data/NAF" / a_name) << json{
			{ "animations", json::array({ { { "samplers", json::array({ { { "input", 0 } } }) } } }) },
			{ "accessors", json::array({ { { "max", json::array({ a_seconds }) } } }) }
		}.dump();
	}
	void SetClips(std::initializer_list<std::string> files)
    {
        clips.clear();
        for (const auto& file : files) clips.push_back({Durations::KeyFor(file, ""), file, ""});
    }
    void Scan() { Cache::Scan(clips, &OnChanged); }
    std::optional<float> Cached(const char* file) { return Cache::Lookup(Durations::KeyFor(file, "")); }

	void RecordWhileBlocked(Gate& a_gate, const char* a_file, float a_seconds)
	{
		Check(a_gate.Wait(), "worker reached the IO boundary");
		auto record = std::async(std::launch::async, [=] {
			Durations::Record(a_file, "", a_seconds);
			Cache::RefreshDecoded();
            return Durations::Lookup(a_file, "") == a_seconds;
		});
		Check(record.wait_for(2s) == std::future_status::ready, "playback recording and lookup do not wait for IO");
		a_gate.Release();
		Check(record.get(), "decoded duration is immediately available");
	}
}

// Only external endpoints are replaced; the memory store, scanner, JSON persistence,
// clip normalization and gzip reader are the production implementations.
namespace SFSE::log
{
	std::optional<std::filesystem::path> log_directory()
	{
		++diskCalls;
		diskGate.Block();
		return root / "logs";
	}
}
namespace OSF::API::EngineClient
{
    const OSFEngineAPI& Get()
    {
        static const auto api = [] {
            OSFEngineAPI value{};
            value.ReadClipDurations = [](TextCallback callback, void* context) {
                const auto text = json(Durations::Snapshot()).dump();
                if (delaySnapshot.exchange(false)) snapshotGate.Block();
                callback(text.c_str(), context); return true;
            };
            value.ProbeClipDuration = [](const char* file, const char* anim, const ClipProbeResult* previous, ClipProbeResult* result) {
                *result = Serialization::ClipDurationProbe::Probe(file, anim, previous); return true;
            };
            return value;
        }();
        return api;
    }
    void ReadJson(const char* text, void* context) noexcept { *static_cast<json*>(context) = json::parse(text); }
    bool Post(std::function<void()> task) { Util::NativeMainThreadQueue::Post(std::move(task)); return true; }
    CatalogData& CatalogData::GetSingleton() { static CatalogData value; return value; }
}
namespace OSF::Util::Ba2
{
	std::optional<std::vector<std::uint8_t>> ReadGameFile(std::string_view, std::string_view)
	{
		++archiveCalls;
		probeGate.Block();
		return archiveBytes;
	}
}
namespace OSF::Util::NativeMainThreadQueue
{
	void Post(std::function<void()> a_task)
	{
		std::lock_guard lock(queueMutex);
		commands.push_back(std::move(a_task));
		queued.notify_all();
	}
}

int main()
{
	const auto originalDirectory = std::filesystem::current_path();
	root = std::filesystem::temp_directory_path() /
		("osf-duration-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(root / "Data/NAF");
	std::filesystem::create_directories(root / "OSF");
	std::filesystem::current_path(root);
	WriteGltf("cached.gltf", 3.0f);
	const auto clip = root / "Data/NAF/cached.gltf";
	std::ofstream(root / "OSF/clip-durations.json") << json{
		{ "version", 1 }, { "clips", {
			{ "naf\\decoded.glb|", { { "sec", 99.0f }, { "exact", true } } },
			{ "naf\\cached.gltf|", { { "sec", 3.0f }, { "size", std::filesystem::file_size(clip) },
				{ "mtime", std::filesystem::last_write_time(clip).time_since_epoch().count() }, { "exact", false } } },
			{ "naf\\archive.glb|", { { "sec", 8.0f }, { "exact", true } } },
			{ "invalid|", { { "sec", -1.0f } } }
		} }
	}.dump();

	Durations::Record("naf:decoded.glb", "", 4.0f);
	Durations::Record("naf:decoded.glb", "named", 6.0f);
	Durations::Record("invalid", "", std::numeric_limits<float>::infinity());
	Check(Durations::Lookup("Data/NAF/decoded.glb", "") == 4.0f, "recording preserves normalized clip identity");
	Check(Durations::Lookup("NAF\\decoded.glb", "named") == 6.0f, "animation IDs remain distinct");
	Check(!Durations::Lookup("invalid", ""), "invalid decoded durations are ignored");
	Check(!Durations::Lookup("naf:archive.glb", ""), "lookup does not load persisted values");
	Check(diskCalls == 0 && archiveCalls == 0, "playback alone does not request cache IO or probes");
	Check(ReadCache()["clips"]["naf\\decoded.glb|"]["sec"] == 99.0f, "recording leaves the disk cache unchanged");

	SetClips({ "naf:cached.gltf" });
	diskGate.Arm();
	Scan();
	RecordWhileBlocked(diskGate, "naf:decoded.glb", 5.0f);
	WaitScan();
	Check(notifications == 1 && callbackThread == std::this_thread::get_id(), "disk-only enrichment notifies through the frame queue");
	Check(Durations::Lookup("naf:decoded.glb", "") == 5.0f, "disk merge preserves values decoded before and during loading");
	Check(Cached("naf:cached.gltf") == 3.0f && Cached("naf:archive.glb") == 8.0f,
		"version-1 cache restores fresh probes and exact archive durations");
	Check(ReadCache()["version"] == 1 && ReadCache()["clips"]["naf\\decoded.glb|"]["sec"] == 5.0f,
		"requested scan persists decoded values using the existing format");
	Check(archiveCalls == 0, "only exported unmeasured clips are probed");
    Check(!Durations::Lookup("naf:cached.gltf", "") && !Durations::Lookup("naf:archive.glb", ""), "browser cache never writes probe or disk values into the engine store");

	const auto callsBeforeRecord = diskCalls.load();
	Durations::Record("naf:after.glb", "", 6.0f);
	Check(diskCalls == callsBeforeRecord && !ReadCache()["clips"].contains("naf\\after.glb|"),
		"later playback also waits for explicit catalog demand before persisting");
	WriteGltf("cached.gltf", 7.0f);
	std::filesystem::last_write_time(clip, std::filesystem::last_write_time(clip) + 2s);
	diskGate.Arm();
	Scan();
	RecordWhileBlocked(diskGate, "naf:after-snapshot.glb", 10.0f);
	WaitScan();
	Check(Cached("naf:cached.gltf") == 7.0f, "changed loose files refresh their probed durations");
	Check(ReadCache()["clips"]["naf\\after.glb|"]["sec"] == 6.0f &&
		!ReadCache()["clips"].contains("naf\\after-snapshot.glb|"), "save writes its captured snapshot");

	SetClips({ "naf:cached.gltf", "probe.af" });
	const std::uint16_t frames = 61; // Probe yields 2 seconds, deliberately different from the decode.
	std::memcpy(archiveBytes.data() + 44, &frames, sizeof(frames));
	probeGate.Arm();
	Scan();
	RecordWhileBlocked(probeGate, "probe.af", 11.0f);
	WaitScan();
	Check(notifications == 3, "scan publishes concurrent decoded changes even when probes add nothing");
	Check(Durations::Lookup("probe.af", "") == 11.0f && Cached("probe.af") == 11.0f, "an older probe cannot overwrite a concurrent decoded value");
	Check(ReadCache()["clips"]["naf\\after-snapshot.glb|"]["sec"] == 10.0f,
		"record made during a previous save remains pending and persists on the next scan");

	// Invalidating a bad clip must also respect a decode completed during the read.
	SetClips({ "broken.af" });
	Durations::Record("broken.af", "", 2.0f);
	archiveBytes.resize(63); // Too short for an AF header, but distinct from unknown source size (0).
	probeGate.Arm();
	Scan();
	RecordWhileBlocked(probeGate, "broken.af", 13.0f);
	WaitScan();
	Check(Durations::Lookup("broken.af", "") == 13.0f && Cached("broken.af") == 13.0f, "failed stale probe cannot erase a concurrent decode");
	Check(!Cached("naf:cached.gltf") && Cached("naf:decoded.glb") == 5.0f,
		"pruning removes unreferenced probes but retains decoded durations");

	Durations::Record("overlap.af", "", 1.0f);
	snapshotGate.Arm(); delaySnapshot = true;
	auto oldSnapshot = std::async(std::launch::async, [] { Cache::RefreshDecoded(); });
	Check(snapshotGate.Wait(), "older decoded snapshot is suspended before merging");
	Durations::Record("overlap.af", "", 2.0f);
	Cache::RefreshDecoded();
	snapshotGate.Release(); oldSnapshot.get();
	Check(Cached("overlap.af") == 2.0f, "an out-of-order worker snapshot cannot replace a newer main-thread decode");

	std::filesystem::current_path(originalDirectory);
	std::filesystem::remove_all(root); // Only the unique temporary fixture directory created above.
	return OSF::Test::Finish("clip duration");
}
