#pragma once

// Development-only in-house profiler core (docs/PROFILING.md). Engine-free: no SFSE/RE includes,
// so osf-profiler-test compiles it directly. Instrument code through Profiling/Profile.h, never
// by calling this header from production code paths.
//
// Threading model:
// - Every instrumented thread owns one ThreadState (registered on first use, never freed).
//   Only the owner writes its aggregates and event ring; readers copy them with relaxed atomics.
// - Aggregates reset lazily: Reset() bumps a global epoch and the owner clears its own table
//   at its next record. Capture() ignores threads that have not acknowledged the current epoch.
// - The trace ring is single-writer, wrapping, with a per-slot sequence lock. A reader accepts a
//   slot only when its sequence is even and unchanged across the copy.

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace OSF::Profiling
{
	// Embedded in every profiling build. tools/ReleaseRules.ps1 refuses to package a DLL containing it.
	inline constexpr char kProfilingBuildMarker[] = "OSFAnimationProfilingBuild";

	inline constexpr std::uint32_t kMaxDescs = 512;          // registered sites per process (id 0 is invalid)
	inline constexpr std::uint32_t kMaxThreads = 256;        // instrumented threads per process
	inline constexpr std::uint32_t kHistogramBuckets = 32;   // bucket b counts durations in [2^(b-1), 2^b) ticks
	inline constexpr std::uint32_t kDefaultRingEvents = 1u << 17;
	inline constexpr double        kGapFrameMs = 250.0;      // longer frames are loads/pauses, not frame time

	enum class Mode : std::uint8_t
	{
		kOff,    // nothing recorded except frame times
		kStats,  // per-thread aggregates + per-frame OSF time
		kTrace   // stats + per-thread event rings for a Chrome/Perfetto timeline
	};

	enum class Kind : std::uint8_t
	{
		kScope,
		kWait,     // OSF_PROFILE_LOCK construction time
		kCounter,
		kGauge
	};

	struct ScopeDesc
	{
		const char*                name;
		const char*                file;
		int                        line;
		std::atomic<std::uint32_t> id{ 0 };  // 0 unresolved, UINT32_MAX dropped (registry full)

		constexpr ScopeDesc(const char* a_name, const char* a_file, int a_line) noexcept :
			name(a_name), file(a_file), line(a_line)
		{}
	};

	namespace detail
	{
		struct ThreadState;

		inline std::atomic<Mode>          g_mode{ Mode::kOff };
		inline std::atomic<std::uint64_t> g_frameIndex{ 0 };

		std::uint64_t Ticks() noexcept;  // raw QueryPerformanceCounter
		void          RecordIntervalSlow(ScopeDesc& a_desc, std::uint64_t a_begin, std::uint64_t a_end) noexcept;
		void          CounterSlow(ScopeDesc& a_desc, std::int64_t a_delta) noexcept;
		void          GaugeSlow(ScopeDesc& a_desc, std::int64_t a_value) noexcept;

		// Nearest-rank percentile over an ascending array; 0 for an empty one. Exposed for tests.
		double NearestRank(std::span<const double> a_sorted, double a_percent) noexcept;

		// Defined in AllocationHooks.cpp: this thread's global operator new/delete activity.
		extern thread_local std::uint64_t t_allocCount;
		extern thread_local std::uint64_t t_allocBytes;
		extern thread_local std::uint64_t t_freeCount;
	}

	inline bool Enabled() noexcept { return detail::g_mode.load(std::memory_order_relaxed) != Mode::kOff; }

	// Raw ticks while enabled, 0 while off (so OSF_PROFILE_LOCK costs one branch when off).
	inline std::uint64_t Now() noexcept { return Enabled() ? detail::Ticks() : 0; }

	class ScopeGuard
	{
	public:
		explicit ScopeGuard(ScopeDesc& a_desc) noexcept
		{
			if (Enabled()) {
				Begin(a_desc);
			}
		}
		~ScopeGuard()
		{
			if (_thread) {
				End();
			}
		}
		ScopeGuard(const ScopeGuard&) = delete;
		ScopeGuard& operator=(const ScopeGuard&) = delete;

	private:
		void Begin(ScopeDesc& a_desc) noexcept;
		void End() noexcept;

		detail::ThreadState* _thread = nullptr;
		std::uint64_t        _begin = 0;
		std::uint64_t        _frame = 0;
		std::uint64_t        _allocs = 0;
		std::uint64_t        _bytes = 0;
		std::uint32_t        _id = 0;
		std::uint8_t         _depth = 0;
	};

	// A lock wait measured by OSF_PROFILE_LOCK; it does not change the nesting depth.
	inline void RecordInterval(ScopeDesc& a_desc, std::uint64_t a_begin, std::uint64_t a_end) noexcept
	{
		if (a_begin != 0 && a_end >= a_begin) {
			detail::RecordIntervalSlow(a_desc, a_begin, a_end);
		}
	}

	inline void Counter(ScopeDesc& a_desc, std::int64_t a_delta) noexcept
	{
		if (Enabled()) {
			detail::CounterSlow(a_desc, a_delta);
		}
	}

	inline void Gauge(ScopeDesc& a_desc, std::int64_t a_value) noexcept
	{
		if (Enabled()) {
			detail::GaugeSlow(a_desc, a_value);
		}
	}

	// Returns a callable that runs a_fn inside a scope named by a_desc.
	template <class F>
	auto Tagged(ScopeDesc& a_desc, F a_fn)
	{
		return [desc = &a_desc, fn = std::move(a_fn)](auto&&... a_args) mutable -> decltype(auto) {
			const ScopeGuard guard{ *desc };
			return fn(std::forward<decltype(a_args)>(a_args)...);
		};
	}

	// Frame boundary. Call once per game frame from one thread (the frame-maintenance hook). Frame
	// times are recorded in every mode, so an "off" capture is the overhead baseline.
	void FrameMark() noexcept;

	// Names the calling thread in captures (and for debuggers via SetThreadDescription).
	void NameCurrentThread(const char* a_name) noexcept;

	// Instant event on the timeline (trace output) with a free-form label.
	void Mark(std::string_view a_label);

	Mode CurrentMode() noexcept;
	void SetMode(Mode a_mode) noexcept;

	// Starts a fresh capture window: aggregates, frames, marks and visible ring events restart.
	// Registrations and thread names are kept.
	void Reset() noexcept;

	// Events per thread ring (rounded up to a power of two, 1024..1<<22). Rings allocated before the
	// change keep their size until the next Reset().
	void          SetRingCapacity(std::uint32_t a_events) noexcept;
	std::uint32_t RingCapacity() noexcept;

	std::string_view ModeName(Mode a_mode) noexcept;

	struct DescInfo
	{
		std::string name;
		std::string category;
		std::string site;  // file:line
		Kind        kind = Kind::kScope;
	};

	struct AggCopy
	{
		std::uint32_t                                id = 0;
		std::uint64_t                                count = 0;
		std::int64_t                                 total = 0;
		std::int64_t                                 min = 0;
		std::int64_t                                 max = 0;
		std::int64_t                                 last = 0;
		std::uint64_t                                lastAt = 0;
		std::uint64_t                                allocs = 0;
		std::uint64_t                                allocBytes = 0;
		std::array<std::uint32_t, kHistogramBuckets> histogram{};
	};

	struct EventCopy
	{
		std::uint64_t begin = 0;
		std::uint64_t end = 0;    // counter delta / gauge value for kCounter / kGauge
		std::uint32_t tid = 0;
		std::uint32_t id = 0;
		std::uint32_t allocs = 0;
		std::uint32_t allocBytes = 0;
		Kind          kind = Kind::kScope;
		std::uint8_t  depth = 0;
	};

	struct ThreadCopy
	{
		std::uint32_t        tid = 0;  // registration order, 1-based
		std::uint32_t        osTid = 0;
		std::string          name;
		bool                 isMain = false;
		bool                 exited = false;
		bool                 current = false;  // acknowledged the capture window (aggregates valid)
		std::uint64_t        events = 0;       // ring events accepted into this capture
		std::uint64_t        torn = 0;         // ring slots skipped mid-write
		bool                 wrapped = false;  // ring overwrote events inside this window
		std::vector<AggCopy> aggs;
	};

	struct FrameCopy
	{
		std::uint64_t index = 0;
		std::uint64_t begin = 0;
		std::uint64_t ticks = 0;
		std::uint64_t osfTicks = 0;
		bool          osfFinal = false;
	};

	struct MarkCopy
	{
		std::uint64_t at = 0;
		std::uint32_t tid = 0;
		std::string   label;
	};

	struct Snapshot
	{
		Mode                    mode = Mode::kOff;
		std::uint64_t           frequency = 0;
		std::uint64_t           startTicks = 0;
		std::uint64_t           endTicks = 0;
		std::uint64_t           startMonotonicNs = 0;  // steady_clock ns at window start (Util::MonotonicNs scale)
		std::string             startedAt;             // local ISO-8601
		std::string             endedAt;
		std::uint32_t           ringCapacity = 0;
		std::uint32_t           droppedDescs = 0;
		std::uint32_t           droppedThreads = 0;
		std::vector<DescInfo>   descs;  // index = id
		std::vector<ThreadCopy> threads;
		std::vector<EventCopy>  events;  // trace mode only; unsorted
		std::vector<FrameCopy>  frames;
		std::vector<MarkCopy>   marks;
	};

	// Copies the current window on the calling thread. O(registered sites x threads + ring events).
	Snapshot Capture();

	struct DumpMeta
	{
		std::string label;   // file-name part; sanitized
		std::string reason;  // "command", "scene-end", "world-reset", ...
		std::string plugin = "OSF Animation";
		std::string dllVersion;
		std::string gameVersion;
	};

	struct DumpResult
	{
		std::filesystem::path    stats;
		std::filesystem::path    trace;  // empty unless the snapshot was taken in trace mode
		std::vector<std::string> summary;
		std::string              error;  // non-empty when a file could not be written
	};

	// Writes <stamp>-<label>.stats.json (and .trace.json.gz in trace mode) into a_dir, through
	// temporary files renamed into place. Slow; call it off the game thread.
	DumpResult Write(const Snapshot& a_snapshot, const std::filesystem::path& a_dir, const DumpMeta& a_meta);
}
