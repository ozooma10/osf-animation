// osf-profiler-test: the in-house profiler core, compiled with OSF_PROFILING=1.
// The profiler is process-global, so cases run in order and each starts with Reset()/SetMode().

#include "Profiling/Profile.h"
#include "Util/Gzip.h"

#include "Check.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if !defined(OSF_PROFILING) || !OSF_PROFILING
#	error ProfilerTest.cpp must be compiled with OSF_PROFILING=1
#endif

namespace OSF::Test::ProfilerNoop
{
	int  SideEffects();
	bool TaskIsIdentity();
	bool LocksReallyLock();
}

namespace
{
	using namespace OSF::Profiling;
	using namespace std::chrono_literals;

	volatile char        g_sink = 0;
	void* volatile       g_pointerSink = nullptr;

	struct Totals
	{
		std::uint64_t count = 0;
		std::int64_t  total = 0;
		std::int64_t  min = std::numeric_limits<std::int64_t>::max();
		std::int64_t  max = std::numeric_limits<std::int64_t>::min();
		std::int64_t  last = 0;
		std::uint64_t lastAt = 0;
		std::uint64_t allocs = 0;
		std::uint64_t allocBytes = 0;
		std::uint64_t histogram = 0;
		std::size_t   ids = 0;
	};

	std::vector<std::uint32_t> IdsOf(const Snapshot& a_snapshot, std::string_view a_name)
	{
		std::vector<std::uint32_t> ids;
		for (std::uint32_t id = 1; id < a_snapshot.descs.size(); ++id) {
			if (a_snapshot.descs[id].name == a_name) {
				ids.push_back(id);
			}
		}
		return ids;
	}

	Totals Sum(const Snapshot& a_snapshot, std::string_view a_name)
	{
		Totals     out;
		const auto ids = IdsOf(a_snapshot, a_name);
		out.ids = ids.size();
		for (const auto& thread : a_snapshot.threads) {
			for (const auto& agg : thread.aggs) {
				if (std::find(ids.begin(), ids.end(), agg.id) == ids.end()) {
					continue;
				}
				out.count += agg.count;
				out.total += agg.total;
				out.min = std::min(out.min, agg.min);
				out.max = std::max(out.max, agg.max);
				if (agg.lastAt >= out.lastAt) {
					out.lastAt = agg.lastAt;
					out.last = agg.last;
				}
				out.allocs += agg.allocs;
				out.allocBytes += agg.allocBytes;
				for (const auto bucket : agg.histogram) {
					out.histogram += bucket;
				}
			}
		}
		return out;
	}

	std::vector<EventCopy> EventsOf(const Snapshot& a_snapshot, std::string_view a_name)
	{
		const auto             ids = IdsOf(a_snapshot, a_name);
		std::vector<EventCopy> out;
		for (const auto& event : a_snapshot.events) {
			if (std::find(ids.begin(), ids.end(), event.id) != ids.end()) {
				out.push_back(event);
			}
		}
		return out;
	}

	std::int64_t TicksPerMs(const Snapshot& a_snapshot) { return static_cast<std::int64_t>(a_snapshot.frequency / 1000); }

	void Begin(Mode a_mode)
	{
		SetMode(a_mode);
		Reset();
	}

	void NoopContract()
	{
		CHECK(OSF::Test::ProfilerNoop::SideEffects() == 0);
		CHECK(OSF::Test::ProfilerNoop::TaskIsIdentity());
		CHECK(OSF::Test::ProfilerNoop::LocksReallyLock());
	}

	void Registration()
	{
		Begin(Mode::kStats);
		static ScopeDesc first{ "Test.Reg.Same", __FILE__, __LINE__ };
		static ScopeDesc second{ "Test.Reg.Same", __FILE__, __LINE__ };
		{
			const ScopeGuard a{ first };
			const ScopeGuard b{ second };
		}
		{
			OSF_PROFILE_SCOPE("NoCategory");
		}
		CHECK(first.id.load() != 0);
		CHECK(second.id.load() != 0);
		CHECK(first.id.load() != second.id.load());
		const Snapshot snapshot = Capture();
		CHECK(snapshot.descs[first.id.load()].category == "Test");
		CHECK(snapshot.descs[first.id.load()].site.find("ProfilerTest.cpp") != std::string::npos);
		const auto none = IdsOf(snapshot, "NoCategory");
		CHECK(none.size() == 1 && snapshot.descs[none.front()].category.empty());
		CHECK(Sum(snapshot, "Test.Reg.Same").count == 2);
	}

	void OffRecordsNothing()
	{
		Begin(Mode::kOff);
		CHECK(Now() == 0);
		{
			OSF_PROFILE_SCOPE("Test.Off.Scope");
			OSF_PROFILE_COUNTER("Test.Off.Counter", 1);
			OSF_PROFILE_GAUGE("Test.Off.Gauge", 1);
		}
		FrameMark();
		std::this_thread::sleep_for(2ms);
		FrameMark();
		const Snapshot snapshot = Capture();
		CHECK(IdsOf(snapshot, "Test.Off.Scope").empty());
		CHECK(IdsOf(snapshot, "Test.Off.Counter").empty());
		CHECK(snapshot.frames.size() >= 1);  // frames are timed even when off: the overhead baseline
	}

	void Aggregates()
	{
		Begin(Mode::kStats);
		for (int i = 0; i < 1000; ++i) {
			OSF_PROFILE_SCOPE("Test.Agg");
			if (i % 400 == 0) {
				std::this_thread::sleep_for(2ms);
			}
		}
		std::mutex lock;
		{
			OSF_PROFILE_SCOPE("Test.Agg.Outer");
			OSF_PROFILE_LOCK(std::unique_lock, held, "Test.Agg.Wait", lock);
		}
		int hits = 0;
		auto task = OSF_PROFILE_TASK("Test.Task", [&hits] { ++hits; });
		task();
		std::function<void()> stored = OSF_PROFILE_TASK("Test.Task.Stored", [&hits] { ++hits; });
		stored();
		const int value = OSF_PROFILE_CALL("Test.Call", hits * 10);
		std::vector<int> moved{ 1, 2, 3 };
		const auto taken = OSF_PROFILE_CALL("Test.Call.Move", std::move(moved));
		const Snapshot snapshot = Capture();
		const Totals   agg = Sum(snapshot, "Test.Agg");
		CHECK(agg.count == 1000);
		CHECK(agg.min <= agg.total / 1000 && agg.total / 1000 <= agg.max);
		CHECK(agg.max >= 2 * TicksPerMs(snapshot) * 9 / 10);
		CHECK(agg.histogram == agg.count);
		const auto waitIds = IdsOf(snapshot, "Test.Agg.Wait");
		CHECK(waitIds.size() == 1 && snapshot.descs[waitIds.front()].kind == Kind::kWait);
		CHECK(Sum(snapshot, "Test.Agg.Wait").count == 1);
		CHECK(hits == 2);
		CHECK(Sum(snapshot, "Test.Task").count == 1);
		CHECK(Sum(snapshot, "Test.Task.Stored").count == 1);
		CHECK(value == 20 && Sum(snapshot, "Test.Call").count == 1);
		CHECK(taken.size() == 3 && Sum(snapshot, "Test.Call.Move").count == 1);
	}

	void NestingAndFrames()
	{
		Begin(Mode::kTrace);
		{
			OSF_PROFILE_SCOPE("Test.Nest.0");
			{
				OSF_PROFILE_SCOPE("Test.Nest.1");
				std::mutex lock;
				OSF_PROFILE_LOCK(std::lock_guard, held, "Test.Nest.Wait", lock);
				{
					OSF_PROFILE_SCOPE("Test.Nest.2");
				}
			}
		}
		Snapshot snapshot = Capture();
		const auto depth = [&](std::string_view a_name) {
			const auto events = EventsOf(snapshot, a_name);
			return events.size() == 1 ? static_cast<int>(events.front().depth) : -1;
		};
		CHECK(depth("Test.Nest.0") == 0);
		CHECK(depth("Test.Nest.1") == 1);
		CHECK(depth("Test.Nest.2") == 2);
		CHECK(depth("Test.Nest.Wait") == 2);  // a wait records at the current depth without nesting

		// Root scopes only: the outer duration is the frame's OSF time, the inner one is not added again.
		Begin(Mode::kStats);
		FrameMark();
		{
			OSF_PROFILE_SCOPE("Test.Frame.Outer");
			std::this_thread::sleep_for(3ms);
			OSF_PROFILE_SCOPE("Test.Frame.Inner");
			std::this_thread::sleep_for(1ms);
		}
		FrameMark();
		FrameMark();
		snapshot = Capture();
		const Totals outer = Sum(snapshot, "Test.Frame.Outer");
		CHECK(outer.count == 1);
		CHECK(!snapshot.frames.empty() && snapshot.frames.front().osfFinal);
		CHECK(!snapshot.frames.empty() && static_cast<std::int64_t>(snapshot.frames.front().osfTicks) == outer.total);
	}

	void RingWrap()
	{
		SetRingCapacity(1000);  // rounds up to 1024
		CHECK(RingCapacity() == 1024);
		Begin(Mode::kTrace);
		for (int i = 0; i < 5000; ++i) {
			OSF_PROFILE_SCOPE("Test.Wrap");
		}
		const Snapshot snapshot = Capture();
		const auto     events = EventsOf(snapshot, "Test.Wrap");
		CHECK(events.size() == 1024);
		bool ordered = true;
		for (const auto& event : events) {
			ordered = ordered && event.end >= event.begin;
		}
		CHECK(ordered);
		bool wrapped = false;
		for (const auto& thread : snapshot.threads) {
			wrapped = wrapped || (thread.events == 1024 && thread.wrapped);
		}
		CHECK(wrapped);
	}

	void ConcurrentCapture()
	{
		Begin(Mode::kTrace);
		std::atomic<bool> stop{ false };
		std::thread       writer([&] {
            OSF_PROFILE_THREAD_NAME("concurrent-writer");
            while (!stop.load(std::memory_order_relaxed)) {
                OSF_PROFILE_SCOPE("Test.Concurrent");
                OSF_PROFILE_GAUGE("Test.Concurrent.Gauge", 7);
            }
        });
		bool          valid = true;
		std::uint64_t seen = 0;
		for (int i = 0; i < 50; ++i) {
			const Snapshot snapshot = Capture();
			for (const auto& thread : snapshot.threads) {
				valid = valid && thread.events <= snapshot.ringCapacity;
			}
			for (const auto& event : snapshot.events) {
				valid = valid && event.id > 0 && event.id < snapshot.descs.size() && event.depth < 64;
				if (event.kind == Kind::kScope) {
					valid = valid && event.end >= event.begin;
				}
				if (event.kind == Kind::kGauge) {
					valid = valid && event.end == 7;
				}
			}
			seen += EventsOf(snapshot, "Test.Concurrent").size();
			std::this_thread::sleep_for(4ms);
		}
		stop.store(true);
		writer.join();
		CHECK(valid);
		CHECK(seen > 0);
	}

	void ThreadLifecycle()
	{
		Begin(Mode::kStats);
		std::vector<std::thread> workers;
		for (int i = 0; i < 8; ++i) {
			workers.emplace_back([i] {
				if (i % 2 == 0) {
					OSF_PROFILE_THREAD_NAME(i == 0 ? "worker-0" : "worker-even");
				}
				for (int n = 0; n < 100; ++n) {
					OSF_PROFILE_SCOPE("Test.Thread");
				}
			});
		}
		for (auto& worker : workers) {
			worker.join();
		}
		const Snapshot snapshot = Capture();
		CHECK(Sum(snapshot, "Test.Thread").count == 800);
		std::size_t exited = 0;
		bool        named = false;
		for (const auto& thread : snapshot.threads) {
			exited += thread.exited ? 1 : 0;
			named = named || thread.name == "worker-0";
		}
		CHECK(exited >= 8);
		CHECK(named);
	}

	struct alignas(64) Aligned
	{
		char bytes[64];
	};

	void Allocations()
	{
		Begin(Mode::kStats);
		{
			OSF_PROFILE_SCOPE("Test.Alloc");
			std::vector<char> buffer(100000);
			buffer[99999] = 1;
			g_sink = buffer[99999];
			{
				OSF_PROFILE_SCOPE("Test.Alloc.Inner");
				auto* aligned = new Aligned{};
				g_pointerSink = aligned;
				delete aligned;
			}
		}
		const Snapshot snapshot = Capture();
		const Totals   outer = Sum(snapshot, "Test.Alloc");
		const Totals   inner = Sum(snapshot, "Test.Alloc.Inner");
		CHECK(outer.allocs >= 2);  // nested allocations count toward the parent
		CHECK(outer.allocBytes >= 100000 + sizeof(Aligned));
		CHECK(inner.allocs == 1);
		CHECK(inner.allocBytes == sizeof(Aligned));
		CHECK(reinterpret_cast<std::uintptr_t>(g_pointerSink) % alignof(Aligned) == 0);

		bool threw = false;
		try {
			const std::size_t huge = std::numeric_limits<std::size_t>::max() / 4;
			g_pointerSink = ::operator new(huge);
		} catch (const std::bad_alloc&) {
			threw = true;
		}
		CHECK(threw);
		CHECK(::operator new(std::numeric_limits<std::size_t>::max() / 4, std::nothrow) == nullptr);
	}

	void LockWait()
	{
		Begin(Mode::kStats);
		std::mutex        lock;
		std::atomic<bool> held{ false };
		std::thread       holder([&] {
            std::lock_guard guard{ lock };
            held.store(true);
            std::this_thread::sleep_for(30ms);
        });
		while (!held.load()) {
			std::this_thread::yield();
		}
		{
			OSF_PROFILE_LOCK(std::unique_lock, waited, "Test.LockWait", lock);
		}
		holder.join();
		std::shared_mutex shared;
		std::mutex        other;
		{
			OSF_PROFILE_LOCK(std::shared_lock, reader, "Test.LockWait.Shared", shared);
		}
		{
			OSF_PROFILE_LOCK(std::scoped_lock, both, "Test.LockWait.Scoped", lock, other);
		}
		const Snapshot snapshot = Capture();
		CHECK(Sum(snapshot, "Test.LockWait").max >= 20 * TicksPerMs(snapshot));
		CHECK(Sum(snapshot, "Test.LockWait.Shared").count == 1);
		CHECK(Sum(snapshot, "Test.LockWait.Scoped").count == 1);
	}

	void Percentiles()
	{
		std::vector<double> values;
		for (int i = 1; i <= 100; ++i) {
			values.push_back(i);
		}
		CHECK(detail::NearestRank(values, 50) == 50);
		CHECK(detail::NearestRank(values, 95) == 95);
		CHECK(detail::NearestRank(values, 99) == 99);
		CHECK(detail::NearestRank(values, 100) == 100);
		CHECK(detail::NearestRank(values, 0) == 1);
		CHECK(detail::NearestRank({}, 50) == 0);
		const std::vector<double> five{ 1, 2, 3, 4, 5 };
		CHECK(detail::NearestRank(five, 50) == 3);
		CHECK(detail::NearestRank(five, 95) == 5);
	}

	void CountersAndGauges()
	{
		Begin(Mode::kTrace);
		std::vector<std::thread> threads;
		for (int t = 0; t < 2; ++t) {
			threads.emplace_back([] {
				for (int i = 0; i < 100; ++i) {
					OSF_PROFILE_COUNTER("Test.Counter", 3);
				}
			});
		}
		for (auto& thread : threads) {
			thread.join();
		}
		for (const int value : { 5, -2, 9 }) {
			OSF_PROFILE_GAUGE("Test.Gauge", value);
		}
		const Snapshot snapshot = Capture();
		const Totals   counter = Sum(snapshot, "Test.Counter");
		CHECK(counter.total == 600);
		CHECK(counter.count == 200);
		const Totals gauge = Sum(snapshot, "Test.Gauge");
		CHECK(gauge.min == -2 && gauge.max == 9 && gauge.last == 9 && gauge.count == 3);
		const auto counterEvents = EventsOf(snapshot, "Test.Counter");
		CHECK(counterEvents.size() == 200 && counterEvents.front().kind == Kind::kCounter);
	}

	// OSF_PROFILER_TEST_OUTPUT keeps the dump files (sample input for the tools/profiling scripts).
	std::string KeepDirectory()
	{
		char*       value = nullptr;
		std::size_t size = 0;
		std::string out;
		if (_dupenv_s(&value, &size, "OSF_PROFILER_TEST_OUTPUT") == 0 && value) {
			out = value;
		}
		std::free(value);
		return out;
	}

	std::filesystem::path TempDirectory()
	{
		if (const auto keep = KeepDirectory(); !keep.empty()) {
			std::filesystem::create_directories(keep);
			return keep;
		}
		std::random_device random;
		const auto         dir = std::filesystem::temp_directory_path() /
		                 ("osf-profiler-test-" + std::to_string(random()) + std::to_string(random()));
		std::filesystem::create_directories(dir);
		return dir;
	}

	std::string ReadAll(const std::filesystem::path& a_path)
	{
		std::ifstream file(a_path, std::ios::binary);
		return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
	}

	void Dump()
	{
		SetRingCapacity(kDefaultRingEvents);
		Begin(Mode::kTrace);
		FrameMark();
		{
			OSF_PROFILE_SCOPE("Test.Dump.Scope");
			OSF_PROFILE_COUNTER("Test.Dump.Counter", 2);
			OSF_PROFILE_GAUGE("Test.Dump.Gauge", 4);
			std::this_thread::sleep_for(1ms);
		}
		Mark("mid \"quoted\"");
		FrameMark();
		std::this_thread::sleep_for(300ms);  // a gap frame (loading screen / pause)
		FrameMark();
		FrameMark();
		const Snapshot snapshot = Capture();

		const auto dir = TempDirectory();
		DumpMeta   meta;
		meta.label = "unit test";
		meta.reason = "test";
		meta.dllVersion = "0.0.0";
		meta.gameVersion = "1.16.244.0";
		const DumpResult result = Write(snapshot, dir, meta);
		CHECK(result.error.empty());
		CHECK(std::filesystem::exists(result.stats));
		CHECK(std::filesystem::exists(result.trace));
		CHECK(result.stats.filename().string().find("unit-test") != std::string::npos);
		bool partial = false;
		for (const auto& entry : std::filesystem::directory_iterator(dir)) {
			partial = partial || entry.path().string().ends_with(".partial");
		}
		CHECK(!partial);

		auto stats = nlohmann::json::parse(ReadAll(result.stats), nullptr, false);
		CHECK(!stats.is_discarded());
		if (!stats.is_discarded()) {
			for (const char* key : { "schema", "plugin", "build", "mode", "label", "capture", "frames", "scopes", "counters", "threads", "allocations" }) {
				if (!CHECK(stats.contains(key))) {
					std::cerr << "  missing stats key " << key << '\n';
				}
			}
			CHECK(stats.value("build", "") == kProfilingBuildMarker);
			CHECK(stats["frames"].value("gaps", 0) >= 1);
			CHECK(stats["frames"]["frameMs"].contains("p95"));
			bool hasScope = false;
			for (const auto& scope : stats["scopes"]) {
				hasScope = hasScope || (scope.value("name", "") == "Test.Dump.Scope" && scope.value("count", 0) == 1);
			}
			CHECK(hasScope);
		}

		const auto raw = ReadAll(result.trace);
		std::vector<std::byte> bytes(raw.size());
		std::memcpy(bytes.data(), raw.data(), raw.size());
		const auto inflated = OSF::Util::MaybeGunzip(std::move(bytes));
		CHECK(inflated.has_value() && raw.size() > 2 && static_cast<unsigned char>(raw[0]) == 0x1f);
		if (inflated) {
			const std::string text(reinterpret_cast<const char*>(inflated->data()), inflated->size());
			auto              trace = nlohmann::json::parse(text, nullptr, false);
			CHECK(!trace.is_discarded());
			if (!trace.is_discarded()) {
				CHECK(trace["otherData"].value("qpcFrequency", 0ull) > 0);
				std::size_t threadNames = 0;
				bool        hasX = false, hasC = false, hasFrame = false, hasMark = false;
				for (const auto& event : trace["traceEvents"]) {
					const auto ph = event.value("ph", "");
					const auto name = event.value("name", "");
					threadNames += ph == "M" && name == "thread_name" ? 1 : 0;
					hasX = hasX || (ph == "X" && name == "Test.Dump.Scope");
					hasC = hasC || (ph == "C" && name == "Test.Dump.Counter");
					hasFrame = hasFrame || (ph == "X" && name.starts_with("Frame #"));
					hasMark = hasMark || (ph == "i" && name == "mark: mid \"quoted\"");
				}
				CHECK(threadNames == snapshot.threads.size() + 1);  // + the Frames pseudo-thread
				CHECK(hasX);
				CHECK(hasC);
				CHECK(hasFrame);
				CHECK(hasMark);
			}
		}

		const std::vector<std::string_view> prefixes{ "dump 'unit-test'", "frame ms", "top by total", "top by max", "alloc hot spots", "wrote ", "wrote " };
		CHECK(result.summary.size() == prefixes.size());
		for (std::size_t i = 0; i < prefixes.size() && i < result.summary.size(); ++i) {
			if (!CHECK(result.summary[i].starts_with(prefixes[i]))) {
				std::cerr << "  summary line: " << result.summary[i] << '\n';
			}
		}

		// Stats mode writes no trace.
		Begin(Mode::kStats);
		{
			OSF_PROFILE_SCOPE("Test.Dump.Stats");
		}
		const DumpResult statsOnly = Write(Capture(), dir, meta);
		CHECK(statsOnly.error.empty() && !statsOnly.stats.empty() && statsOnly.trace.empty());
		CHECK(statsOnly.stats != result.stats);  // same second, same label: a unique name

		if (KeepDirectory().empty()) {
			std::error_code ignored;
			std::filesystem::remove_all(dir, ignored);
		}
	}

	void ResetSemantics()
	{
		Begin(Mode::kStats);
		NameCurrentThread("profiler-test-main");
		{
			OSF_PROFILE_SCOPE("Test.Reset");
		}
		Mark("before reset");
		FrameMark();
		FrameMark();
		Reset();
		const Snapshot snapshot = Capture();
		CHECK(Sum(snapshot, "Test.Reset").count == 0);  // this thread has not recorded since the reset
		CHECK(IdsOf(snapshot, "Test.Reset").size() == 1);  // registrations survive
		CHECK(snapshot.frames.empty());
		CHECK(snapshot.marks.empty());
		bool named = false;
		for (const auto& thread : snapshot.threads) {
			named = named || thread.name == "profiler-test-main";
		}
		CHECK(named);
		{
			OSF_PROFILE_SCOPE("Test.Reset");
		}
		CHECK(Sum(Capture(), "Test.Reset").count == 1);
	}

	void OverheadReport()
	{
		constexpr int kIterations = 1'000'000;
		for (const Mode mode : { Mode::kOff, Mode::kStats, Mode::kTrace }) {
			Begin(mode);
			const auto start = std::chrono::steady_clock::now();
			for (int i = 0; i < kIterations; ++i) {
				OSF_PROFILE_SCOPE("Test.Overhead");
			}
			const auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
			std::cout << "profiler overhead (" << ModeName(mode) << "): " << ns / kIterations << " ns per scope\n";
		}
		SetMode(Mode::kOff);
	}

	// Last: fills the descriptor registry.
	void RegistryCap()
	{
		Begin(Mode::kStats);
		std::vector<std::unique_ptr<ScopeDesc>> descs;
		for (std::uint32_t i = 0; i < kMaxDescs + 16; ++i) {
			descs.push_back(std::make_unique<ScopeDesc>("Test.Cap", __FILE__, static_cast<int>(i)));
			Counter(*descs.back(), 1);
		}
		const Snapshot snapshot = Capture();
		CHECK(snapshot.droppedDescs > 0);
		CHECK(descs.back()->id.load() == std::numeric_limits<std::uint32_t>::max());
		CHECK(snapshot.descs.size() == kMaxDescs);
		Counter(*descs.back(), 1);  // a dropped site is a no-op, not a retry
		CHECK(Capture().droppedDescs == snapshot.droppedDescs);
		SetMode(Mode::kOff);
	}
}

int main()
{
	NoopContract();
	Registration();
	OffRecordsNothing();
	Aggregates();
	NestingAndFrames();
	RingWrap();
	ConcurrentCapture();
	ThreadLifecycle();
	Allocations();
	LockWait();
	Percentiles();
	CountersAndGauges();
	Dump();
	ResetSemantics();
	OverheadReport();
	RegistryCap();
	return OSF::Test::Finish("profiler");
}
