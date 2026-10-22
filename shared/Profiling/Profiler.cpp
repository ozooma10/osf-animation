#include "Profiling/Profiler.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <zlib.h>

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

namespace OSF::Profiling
{
	namespace detail
	{
		namespace
		{
			constexpr std::uint32_t kDroppedId = std::numeric_limits<std::uint32_t>::max();
			constexpr std::size_t   kMaxFrames = 1u << 20;
			constexpr std::size_t   kMaxMarks = 4096;

			struct Agg
			{
				std::atomic<std::uint64_t> count{ 0 };
				std::atomic<std::int64_t>  total{ 0 };
				std::atomic<std::int64_t>  min{ std::numeric_limits<std::int64_t>::max() };
				std::atomic<std::int64_t>  max{ std::numeric_limits<std::int64_t>::min() };
				std::atomic<std::int64_t>  last{ 0 };
				std::atomic<std::uint64_t> lastAt{ 0 };
				std::atomic<std::uint64_t> allocs{ 0 };
				std::atomic<std::uint64_t> allocBytes{ 0 };
				std::array<std::atomic<std::uint32_t>, kHistogramBuckets> histogram{};
			};

			// One trace record. Every field is a relaxed atomic so a concurrent reader never races
			// on plain memory; the sequence word makes torn copies detectable.
			struct Slot
			{
				std::atomic<std::uint32_t> seq{ 0 };  // ((lap + 1) << 1) when complete, odd while writing
				std::atomic<std::uint16_t> id{ 0 };
				std::atomic<std::uint8_t>  kind{ 0 };
				std::atomic<std::uint8_t>  depth{ 0 };
				std::atomic<std::uint64_t> begin{ 0 };
				std::atomic<std::uint64_t> end{ 0 };
				std::atomic<std::uint32_t> allocs{ 0 };
				std::atomic<std::uint32_t> allocBytes{ 0 };
			};
			static_assert(sizeof(Slot) == 32);

			struct Ring
			{
				explicit Ring(std::uint32_t a_capacity) :
					capacity(a_capacity),
					shift(static_cast<std::uint32_t>(std::countr_zero(a_capacity))),
					slots(new Slot[a_capacity])
				{}

				const std::uint32_t        capacity;  // power of two
				const std::uint32_t        shift;
				std::unique_ptr<Slot[]>    slots;
				std::atomic<std::uint64_t> head{ 0 };  // owner writes; total records ever appended
			};

			struct DescEntry
			{
				const char* name = nullptr;
				const char* file = nullptr;
				int         line = 0;
				Kind        kind = Kind::kScope;
			};

			struct FrameRecord
			{
				std::uint64_t index = 0;
				std::uint64_t begin = 0;
				std::uint64_t ticks = 0;
				std::uint64_t osfTicks = 0;
				bool          osfFinal = false;
			};

			struct MarkRecord
			{
				std::uint64_t at = 0;
				std::uint32_t tid = 0;
				std::string   label;
			};
		}

		struct ThreadState
		{
			std::uint32_t                                   tid = 0;
			std::uint32_t                                   osTid = 0;
			std::atomic<bool>                               exited{ false };
			std::atomic<std::uint64_t>                      epoch{ 0 };  // capture window this table belongs to
			std::uint8_t                                    depth = 0;   // owner only
			std::atomic<Ring*>                              ring{ nullptr };
			std::array<std::atomic<std::uint64_t>, 4>       frameTicks{};  // root-scope ticks per frame slot
			std::array<Agg, kMaxDescs>                      aggs{};
			std::string                                     name;  // g_registryLock
		};

		namespace
		{
			std::mutex                                        g_registryLock;
			std::array<DescEntry, kMaxDescs>                  g_descs{};
			std::atomic<std::uint32_t>                        g_descCount{ 1 };
			std::atomic<std::uint32_t>                        g_droppedDescs{ 0 };
			std::array<std::atomic<ThreadState*>, kMaxThreads> g_threads{};
			std::atomic<std::uint32_t>                        g_threadCount{ 0 };
			std::atomic<std::uint32_t>                        g_droppedThreads{ 0 };
			std::atomic<std::uint64_t>                        g_epoch{ 1 };
			std::atomic<std::uint32_t>                        g_ringCapacity{ kDefaultRingEvents };

			std::mutex                 g_frameLock;  // frames, window bookkeeping, main thread
			std::vector<FrameRecord>   g_frames;
			std::uint64_t              g_frameCounter = 0;
			std::uint64_t              g_prevMark = 0;
			ThreadState*               g_mainThread = nullptr;
			std::atomic<std::uint64_t> g_windowStart{ 0 };
			std::uint64_t              g_windowStartMonotonicNs = 0;
			std::string                g_windowStartedAt;

			std::mutex              g_markLock;
			std::vector<MarkRecord> g_marks;

			thread_local ThreadState* t_state = nullptr;
			thread_local bool         t_registrationFailed = false;

			struct ExitSentinel
			{
				ThreadState* state = nullptr;
				~ExitSentinel()
				{
					if (state) {
						state->exited.store(true, std::memory_order_release);
					}
				}
			};
			thread_local ExitSentinel t_exit;

			std::uint64_t Frequency() noexcept
			{
				static const std::uint64_t frequency = [] {
					LARGE_INTEGER value{};
					QueryPerformanceFrequency(&value);
					return static_cast<std::uint64_t>(value.QuadPart > 0 ? value.QuadPart : 1);
				}();
				return frequency;
			}

			// "C:\...\OSF Animation\src\Animation\Graph.cpp" -> "src/Animation/Graph.cpp".
			std::string RepoRelative(std::string_view a_file)
			{
				std::string path = "/" + std::string(a_file);  // a relative "src\X.cpp" still matches "/src/"
				std::replace(path.begin(), path.end(), '\\', '/');
				std::size_t root = std::string::npos;
				for (const std::string_view marker : { "/src/", "/shared/", "/test/", "/extensions/" }) {
					const auto at = path.rfind(marker);
					if (at != std::string::npos && (root == std::string::npos || at > root)) {
						root = at;
					}
				}
				if (root != std::string::npos) {
					return path.substr(root + 1);
				}
				const auto slash = path.find_last_of('/');
				return slash == std::string::npos ? path : path.substr(slash + 1);
			}

			std::uint64_t MonotonicNs() noexcept
			{
				return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count());
			}

			std::string UtcIso()
			{
				return std::format("{:%FT%T}Z",
					std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now()));
			}

			std::string Narrow(const wchar_t* a_text)
			{
				if (!a_text || !*a_text) {
					return {};
				}
				const int size = WideCharToMultiByte(CP_UTF8, 0, a_text, -1, nullptr, 0, nullptr, nullptr);
				if (size <= 1) {
					return {};
				}
				std::string out(static_cast<std::size_t>(size - 1), '\0');
				WideCharToMultiByte(CP_UTF8, 0, a_text, -1, out.data(), size, nullptr, nullptr);
				return out;
			}

			std::wstring Widen(std::string_view a_text)
			{
				if (a_text.empty()) {
					return {};
				}
				const int size = MultiByteToWideChar(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), nullptr, 0);
				std::wstring out(static_cast<std::size_t>(std::max(size, 0)), L'\0');
				MultiByteToWideChar(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), out.data(), size);
				return out;
			}

			// Windows 10 1607+; resolved dynamically so older SDK declarations don't matter.
			using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
			using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);

			template <class T>
			T Kernel32(const char* a_name) noexcept
			{
				const HMODULE module = GetModuleHandleW(L"kernel32.dll");
				return module ? reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, a_name))) : nullptr;
			}

			std::string OsThreadName()
			{
				static const auto get = Kernel32<GetThreadDescriptionFn>("GetThreadDescription");
				if (!get) {
					return {};
				}
				PWSTR text = nullptr;
				if (FAILED(get(GetCurrentThread(), &text)) || !text) {
					return {};
				}
				std::string name = Narrow(text);
				LocalFree(text);
				return name;
			}

			ThreadState* RegisterThread() noexcept
			{
				std::unique_ptr<ThreadState> state{ new (std::nothrow) ThreadState };
				if (!state) {
					t_registrationFailed = true;
					return nullptr;
				}
				std::string osName;
				try {
					osName = OsThreadName();
				} catch (...) {
				}
				std::lock_guard lock{ g_registryLock };
				const std::uint32_t index = g_threadCount.load(std::memory_order_relaxed);
				if (index >= kMaxThreads) {
					g_droppedThreads.fetch_add(1, std::memory_order_relaxed);
					t_registrationFailed = true;
					return nullptr;
				}
				state->tid = index + 1;
				state->osTid = static_cast<std::uint32_t>(GetCurrentThreadId());
				state->epoch.store(g_epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
				try {
					state->name = osName.empty() ? std::format("thread-{}", state->osTid) : std::move(osName);
				} catch (...) {
				}
				ThreadState* raw = state.release();  // never freed: captures may read it at any time
				g_threads[index].store(raw, std::memory_order_release);
				g_threadCount.store(index + 1, std::memory_order_release);
				t_state = raw;
				t_exit.state = raw;
				return raw;
			}

			ThreadState* Thread() noexcept
			{
				if (ThreadState* state = t_state) {
					return state;
				}
				return t_registrationFailed ? nullptr : RegisterThread();
			}

			std::uint32_t Register(ScopeDesc& a_desc, Kind a_kind) noexcept
			{
				std::lock_guard lock{ g_registryLock };
				if (const std::uint32_t id = a_desc.id.load(std::memory_order_relaxed)) {
					return id;
				}
				const std::uint32_t next = g_descCount.load(std::memory_order_relaxed);
				if (next >= kMaxDescs) {
					g_droppedDescs.fetch_add(1, std::memory_order_relaxed);
					a_desc.id.store(kDroppedId, std::memory_order_release);
					return kDroppedId;
				}
				g_descs[next] = { a_desc.name, a_desc.file, a_desc.line, a_kind };
				g_descCount.store(next + 1, std::memory_order_release);
				a_desc.id.store(next, std::memory_order_release);
				return next;
			}

			// 0 when the registry is full.
			std::uint32_t Resolve(ScopeDesc& a_desc, Kind a_kind) noexcept
			{
				std::uint32_t id = a_desc.id.load(std::memory_order_acquire);
				if (id == 0) {
					id = Register(a_desc, a_kind);
				}
				return id == kDroppedId ? 0 : id;
			}

			// Single-writer updates: plain load/store pairs on relaxed atomics (no lock prefix).
			template <class T, class V>
			void Add(std::atomic<T>& a_value, V a_delta) noexcept
			{
				a_value.store(static_cast<T>(a_value.load(std::memory_order_relaxed) + static_cast<T>(a_delta)), std::memory_order_relaxed);
			}

			void ClearAgg(Agg& a_agg) noexcept
			{
				if (a_agg.count.load(std::memory_order_relaxed) == 0) {
					return;
				}
				a_agg.count.store(0, std::memory_order_relaxed);
				a_agg.total.store(0, std::memory_order_relaxed);
				a_agg.min.store(std::numeric_limits<std::int64_t>::max(), std::memory_order_relaxed);
				a_agg.max.store(std::numeric_limits<std::int64_t>::min(), std::memory_order_relaxed);
				a_agg.last.store(0, std::memory_order_relaxed);
				a_agg.lastAt.store(0, std::memory_order_relaxed);
				a_agg.allocs.store(0, std::memory_order_relaxed);
				a_agg.allocBytes.store(0, std::memory_order_relaxed);
				for (auto& bucket : a_agg.histogram) {
					bucket.store(0, std::memory_order_relaxed);
				}
			}

			// Owner-side lazy reset: the table is cleared by the only thread that writes it.
			void SyncEpoch(ThreadState& a_state) noexcept
			{
				const std::uint64_t epoch = g_epoch.load(std::memory_order_acquire);
				if (a_state.epoch.load(std::memory_order_relaxed) == epoch) {
					return;
				}
				for (auto& agg : a_state.aggs) {
					ClearAgg(agg);
				}
				// A capacity change applies at the window boundary. The old ring is retired, never
				// freed, because a capture on another thread may still be copying it.
				if (Ring* ring = a_state.ring.load(std::memory_order_relaxed);
					ring && ring->capacity != g_ringCapacity.load(std::memory_order_relaxed)) {
					a_state.ring.store(nullptr, std::memory_order_release);
				}
				a_state.epoch.store(epoch, std::memory_order_release);
			}

			std::uint32_t Bucket(std::int64_t a_ticks) noexcept
			{
				const auto width = static_cast<std::uint32_t>(std::bit_width(static_cast<std::uint64_t>(std::max<std::int64_t>(a_ticks, 0))));
				return std::min(width, kHistogramBuckets - 1);
			}

			void Append(ThreadState& a_state, std::uint32_t a_id, Kind a_kind, std::uint8_t a_depth,
				std::uint64_t a_begin, std::uint64_t a_end, std::uint64_t a_allocs, std::uint64_t a_bytes) noexcept
			{
				Ring* ring = a_state.ring.load(std::memory_order_relaxed);
				if (!ring) {
					ring = new (std::nothrow) Ring(g_ringCapacity.load(std::memory_order_relaxed));
					if (!ring || !ring->slots) {
						return;
					}
					a_state.ring.store(ring, std::memory_order_release);
				}
				const std::uint64_t index = ring->head.load(std::memory_order_relaxed);
				Slot&               slot = ring->slots[index & (ring->capacity - 1)];
				const auto          lap = static_cast<std::uint32_t>(index >> ring->shift);
				slot.seq.store((lap << 1) | 1u, std::memory_order_relaxed);
				std::atomic_thread_fence(std::memory_order_release);  // odd sequence precedes the payload
				slot.id.store(static_cast<std::uint16_t>(a_id), std::memory_order_relaxed);
				slot.kind.store(static_cast<std::uint8_t>(a_kind), std::memory_order_relaxed);
				slot.depth.store(a_depth, std::memory_order_relaxed);
				slot.begin.store(a_begin, std::memory_order_relaxed);
				slot.end.store(a_end, std::memory_order_relaxed);
				slot.allocs.store(static_cast<std::uint32_t>(std::min<std::uint64_t>(a_allocs, UINT32_MAX)), std::memory_order_relaxed);
				slot.allocBytes.store(static_cast<std::uint32_t>(std::min<std::uint64_t>(a_bytes, UINT32_MAX)), std::memory_order_relaxed);
				slot.seq.store((lap + 1) << 1, std::memory_order_release);
				ring->head.store(index + 1, std::memory_order_release);
			}

			void RecordDuration(ThreadState& a_state, std::uint32_t a_id, Kind a_kind, std::uint8_t a_depth,
				std::uint64_t a_begin, std::uint64_t a_end, std::uint64_t a_allocs, std::uint64_t a_bytes, std::uint64_t a_frame) noexcept
			{
				SyncEpoch(a_state);
				Agg&       agg = a_state.aggs[a_id];
				const auto ticks = static_cast<std::int64_t>(a_end - a_begin);
				Add(agg.count, 1);
				Add(agg.total, ticks);
				if (ticks < agg.min.load(std::memory_order_relaxed)) {
					agg.min.store(ticks, std::memory_order_relaxed);
				}
				if (ticks > agg.max.load(std::memory_order_relaxed)) {
					agg.max.store(ticks, std::memory_order_relaxed);
				}
				if (a_allocs) {
					Add(agg.allocs, a_allocs);
					Add(agg.allocBytes, a_bytes);
				}
				Add(agg.histogram[Bucket(ticks)], 1);
				if (a_depth == 0) {
					// Shared with FrameMark's exchange, so this one must be a real RMW.
					a_state.frameTicks[a_frame & 3].fetch_add(static_cast<std::uint64_t>(ticks), std::memory_order_relaxed);
				}
				if (g_mode.load(std::memory_order_relaxed) == Mode::kTrace) {
					Append(a_state, a_id, a_kind, a_depth, a_begin, a_end, a_allocs, a_bytes);
				}
			}

			void RecordValue(ThreadState& a_state, std::uint32_t a_id, Kind a_kind, std::int64_t a_value) noexcept
			{
				SyncEpoch(a_state);
				const std::uint64_t at = Ticks();
				Agg&                agg = a_state.aggs[a_id];
				Add(agg.count, 1);
				Add(agg.total, a_value);
				if (a_value < agg.min.load(std::memory_order_relaxed)) {
					agg.min.store(a_value, std::memory_order_relaxed);
				}
				if (a_value > agg.max.load(std::memory_order_relaxed)) {
					agg.max.store(a_value, std::memory_order_relaxed);
				}
				agg.last.store(a_value, std::memory_order_relaxed);
				agg.lastAt.store(at, std::memory_order_relaxed);
				if (g_mode.load(std::memory_order_relaxed) == Mode::kTrace) {
					Append(a_state, a_id, a_kind, a_state.depth, at, static_cast<std::uint64_t>(a_value), 0, 0);
				}
			}

			template <class F>
			void ForEachThread(F&& a_fn)
			{
				const std::uint32_t count = g_threadCount.load(std::memory_order_acquire);
				for (std::uint32_t i = 0; i < count; ++i) {
					if (ThreadState* state = g_threads[i].load(std::memory_order_acquire)) {
						a_fn(*state);
					}
				}
			}
		}

		std::uint64_t Ticks() noexcept
		{
			LARGE_INTEGER value;
			QueryPerformanceCounter(&value);
			return static_cast<std::uint64_t>(value.QuadPart);
		}

		void RecordIntervalSlow(ScopeDesc& a_desc, std::uint64_t a_begin, std::uint64_t a_end) noexcept
		{
			const std::uint32_t id = Resolve(a_desc, Kind::kWait);
			ThreadState*        state = id ? Thread() : nullptr;
			if (state) {
				RecordDuration(*state, id, Kind::kWait, state->depth, a_begin, a_end, 0, 0,
					g_frameIndex.load(std::memory_order_relaxed));
			}
		}

		void CounterSlow(ScopeDesc& a_desc, std::int64_t a_delta) noexcept
		{
			const std::uint32_t id = Resolve(a_desc, Kind::kCounter);
			if (ThreadState* state = id ? Thread() : nullptr) {
				RecordValue(*state, id, Kind::kCounter, a_delta);
			}
		}

		void GaugeSlow(ScopeDesc& a_desc, std::int64_t a_value) noexcept
		{
			const std::uint32_t id = Resolve(a_desc, Kind::kGauge);
			if (ThreadState* state = id ? Thread() : nullptr) {
				RecordValue(*state, id, Kind::kGauge, a_value);
			}
		}

		double NearestRank(std::span<const double> a_sorted, double a_percent) noexcept
		{
			if (a_sorted.empty()) {
				return 0.0;
			}
			const auto n = a_sorted.size();
			const auto rank = static_cast<std::size_t>(std::ceil(std::clamp(a_percent, 0.0, 100.0) / 100.0 * static_cast<double>(n)));
			return a_sorted[std::clamp<std::size_t>(rank, 1, n) - 1];
		}
	}

	using namespace detail;

	void ScopeGuard::Begin(ScopeDesc& a_desc) noexcept
	{
		const std::uint32_t id = Resolve(a_desc, Kind::kScope);
		ThreadState*        state = id ? Thread() : nullptr;
		if (!state) {
			return;
		}
		_thread = state;
		_id = id;
		_depth = state->depth;
		if (state->depth < std::numeric_limits<std::uint8_t>::max()) {
			++state->depth;
		}
		_allocs = t_allocCount;
		_bytes = t_allocBytes;
		_frame = g_frameIndex.load(std::memory_order_relaxed);
		_begin = Ticks();
	}

	void ScopeGuard::End() noexcept
	{
		const std::uint64_t end = Ticks();
		ThreadState&        state = *_thread;
		if (state.depth > _depth) {
			state.depth = _depth;
		}
		RecordDuration(state, _id, Kind::kScope, _depth, _begin, end, t_allocCount - _allocs, t_allocBytes - _bytes, _frame);
	}

	void FrameMark() noexcept
	{
		const std::uint64_t now = Ticks();
		ThreadState*        state = Thread();
		std::lock_guard     lock{ g_frameLock };
		if (!g_mainThread) {
			g_mainThread = state;
		}
		const std::uint64_t index = ++g_frameCounter;
		g_frameIndex.store(index, std::memory_order_relaxed);
		if (g_prevMark != 0 && now > g_prevMark && g_frames.size() < kMaxFrames) {
			try {
				if (g_frames.capacity() == 0) {
					g_frames.reserve(8192);
				}
				g_frames.push_back({ index - 1, g_prevMark, now - g_prevMark, 0, false });
			} catch (...) {
			}
		}
		g_prevMark = now;
		// Root scopes are filed under the frame they began in. Slot (index-2) has had a whole extra
		// frame for job-thread stragglers, so it is final now.
		if (index >= 2) {
			const std::uint64_t target = index - 2;
			std::uint64_t       sum = 0;
			ForEachThread([&](ThreadState& a_thread) {
				sum += a_thread.frameTicks[target & 3].exchange(0, std::memory_order_relaxed);
			});
			for (auto it = g_frames.rbegin(); it != g_frames.rend() && it->index >= target; ++it) {
				if (it->index == target) {
					it->osfTicks = sum;
					it->osfFinal = true;
					break;
				}
			}
		}
	}

	void NameCurrentThread(const char* a_name) noexcept
	{
		if (!a_name) {
			return;
		}
		if (ThreadState* state = Thread()) {
			std::lock_guard lock{ g_registryLock };
			try {
				state->name = a_name;
			} catch (...) {
			}
		}
		static const auto set = Kernel32<SetThreadDescriptionFn>("SetThreadDescription");
		if (set) {
			try {
				set(GetCurrentThread(), Widen(a_name).c_str());
			} catch (...) {
			}
		}
	}

	void Mark(std::string_view a_label)
	{
		const std::uint64_t at = Ticks();
		ThreadState*        state = Thread();
		std::lock_guard     lock{ g_markLock };
		if (g_marks.size() < kMaxMarks) {
			g_marks.push_back({ at, state ? state->tid : 0, std::string(a_label) });
		}
	}

	Mode CurrentMode() noexcept { return g_mode.load(std::memory_order_relaxed); }

	void SetMode(Mode a_mode) noexcept { g_mode.store(a_mode, std::memory_order_relaxed); }

	void Reset() noexcept
	{
		g_epoch.fetch_add(1, std::memory_order_acq_rel);
		{
			std::lock_guard lock{ g_frameLock };
			g_frames.clear();
			g_prevMark = 0;
			ForEachThread([](ThreadState& a_thread) {
				for (auto& slot : a_thread.frameTicks) {
					slot.exchange(0, std::memory_order_relaxed);
				}
			});
			g_windowStart.store(Ticks(), std::memory_order_release);
			g_windowStartMonotonicNs = MonotonicNs();
			try {
				g_windowStartedAt = UtcIso();
			} catch (...) {
			}
		}
		std::lock_guard lock{ g_markLock };
		g_marks.clear();
	}

	void SetRingCapacity(std::uint32_t a_events) noexcept
	{
		const std::uint32_t clamped = std::clamp<std::uint32_t>(a_events, 1024, 1u << 22);
		g_ringCapacity.store(std::bit_ceil(clamped), std::memory_order_relaxed);
	}

	std::uint32_t RingCapacity() noexcept { return g_ringCapacity.load(std::memory_order_relaxed); }

	std::string_view ModeName(Mode a_mode) noexcept
	{
		switch (a_mode) {
		case Mode::kStats:
			return "stats";
		case Mode::kTrace:
			return "trace";
		default:
			return "off";
		}
	}

	Snapshot Capture()
	{
		Snapshot snapshot;
		snapshot.mode = CurrentMode();
		snapshot.frequency = Frequency();
		snapshot.ringCapacity = RingCapacity();
		snapshot.droppedDescs = g_droppedDescs.load(std::memory_order_relaxed);
		snapshot.droppedThreads = g_droppedThreads.load(std::memory_order_relaxed);
		const std::uint64_t windowStart = g_windowStart.load(std::memory_order_acquire);
		snapshot.startTicks = windowStart;
		{
			std::lock_guard lock{ g_frameLock };
			snapshot.startMonotonicNs = g_windowStartMonotonicNs;
			snapshot.startedAt = g_windowStartedAt;
			snapshot.frames.reserve(g_frames.size());
			for (const auto& frame : g_frames) {
				snapshot.frames.push_back({ frame.index, frame.begin, frame.ticks, frame.osfTicks, frame.osfFinal });
			}
		}
		snapshot.endTicks = Ticks();
		snapshot.endedAt = UtcIso();
		if (snapshot.startTicks == 0) {
			// Never reset: the window opened with the first recorded frame (or now).
			snapshot.startTicks = snapshot.frames.empty() ? snapshot.endTicks : snapshot.frames.front().begin;
		}

		{
			const std::uint32_t count = g_descCount.load(std::memory_order_acquire);
			snapshot.descs.resize(count);
			for (std::uint32_t id = 1; id < count; ++id) {
				const DescEntry& entry = g_descs[id];
				DescInfo&        info = snapshot.descs[id];
				info.name = entry.name ? entry.name : "";
				const auto dot = info.name.find('.');
				info.category = dot == std::string::npos ? std::string{} : info.name.substr(0, dot);
				info.site = std::format("{}:{}", RepoRelative(entry.file ? entry.file : ""), entry.line);
				info.kind = entry.kind;
			}
		}

		{
			std::lock_guard lock{ g_markLock };
			for (const auto& mark : g_marks) {
				if (mark.at >= snapshot.startTicks) {
					snapshot.marks.push_back({ mark.at, mark.tid, mark.label });
				}
			}
		}

		const std::uint64_t epoch = g_epoch.load(std::memory_order_acquire);
		const ThreadState*  mainThread = nullptr;
		{
			std::lock_guard lock{ g_frameLock };
			mainThread = g_mainThread;
		}
		ForEachThread([&](ThreadState& a_state) {
			ThreadCopy copy;
			copy.tid = a_state.tid;
			copy.osTid = a_state.osTid;
			copy.isMain = &a_state == mainThread;
			copy.exited = a_state.exited.load(std::memory_order_acquire);
			{
				std::lock_guard lock{ g_registryLock };
				copy.name = a_state.name;
			}
			copy.current = a_state.epoch.load(std::memory_order_acquire) == epoch;
			if (copy.current) {
				for (std::uint32_t id = 1; id < kMaxDescs; ++id) {
					const Agg&          agg = a_state.aggs[id];
					const std::uint64_t count = agg.count.load(std::memory_order_relaxed);
					if (count == 0) {
						continue;
					}
					AggCopy out;
					out.id = id;
					out.count = count;
					out.total = agg.total.load(std::memory_order_relaxed);
					out.min = agg.min.load(std::memory_order_relaxed);
					out.max = agg.max.load(std::memory_order_relaxed);
					out.last = agg.last.load(std::memory_order_relaxed);
					out.lastAt = agg.lastAt.load(std::memory_order_relaxed);
					out.allocs = agg.allocs.load(std::memory_order_relaxed);
					out.allocBytes = agg.allocBytes.load(std::memory_order_relaxed);
					for (std::uint32_t b = 0; b < kHistogramBuckets; ++b) {
						out.histogram[b] = agg.histogram[b].load(std::memory_order_relaxed);
					}
					copy.aggs.push_back(out);
				}
			}
			if (const Ring* ring = a_state.ring.load(std::memory_order_acquire)) {
				const std::uint64_t head = ring->head.load(std::memory_order_acquire);
				const std::uint64_t first = head > ring->capacity ? head - ring->capacity : 0;
				std::uint64_t       oldestKept = 0;
				for (std::uint64_t i = first; i < head; ++i) {
					const Slot&         slot = ring->slots[i & (ring->capacity - 1)];
					const auto          expected = static_cast<std::uint32_t>(((i >> ring->shift) + 1) << 1);
					const std::uint32_t before = slot.seq.load(std::memory_order_acquire);
					if (before != expected) {
						++copy.torn;
						continue;
					}
					EventCopy event;
					event.id = slot.id.load(std::memory_order_relaxed);
					event.kind = static_cast<Kind>(slot.kind.load(std::memory_order_relaxed));
					event.depth = slot.depth.load(std::memory_order_relaxed);
					event.begin = slot.begin.load(std::memory_order_relaxed);
					event.end = slot.end.load(std::memory_order_relaxed);
					event.allocs = slot.allocs.load(std::memory_order_relaxed);
					event.allocBytes = slot.allocBytes.load(std::memory_order_relaxed);
					std::atomic_thread_fence(std::memory_order_acquire);
					if (slot.seq.load(std::memory_order_relaxed) != before) {
						++copy.torn;
						continue;
					}
					if (event.begin < snapshot.startTicks) {
						continue;
					}
					if (oldestKept == 0) {
						oldestKept = event.begin;
					}
					event.tid = a_state.tid;
					snapshot.events.push_back(event);
					++copy.events;
				}
				// The ring lost events from inside this window when it wrapped and kept nothing older.
				copy.wrapped = first > 0 && oldestKept > snapshot.startTicks;
			}
			snapshot.threads.push_back(std::move(copy));
		});
		return snapshot;
	}

	// ---------------------------------------------------------------------------------------------
	// Analysis and output
	// ---------------------------------------------------------------------------------------------

	namespace
	{
		struct Merged
		{
			std::string                                  name;
			std::string                                  category;
			Kind                                         kind = Kind::kScope;
			std::uint64_t                                count = 0;
			std::int64_t                                 total = 0;
			std::int64_t                                 min = std::numeric_limits<std::int64_t>::max();
			std::int64_t                                 max = std::numeric_limits<std::int64_t>::min();
			std::int64_t                                 last = 0;
			std::uint64_t                                lastAt = 0;
			std::uint64_t                                allocs = 0;
			std::uint64_t                                allocBytes = 0;
			std::array<std::uint64_t, kHistogramBuckets> histogram{};
			std::vector<std::string>                     sites;

			struct PerThread
			{
				std::uint32_t tid = 0;
				std::uint64_t count = 0;
				std::int64_t  total = 0;
				std::int64_t  max = 0;
			};
			std::vector<PerThread> threads;
		};

		struct Distribution
		{
			std::size_t count = 0;
			double      mean = 0, p50 = 0, p95 = 0, p99 = 0, max = 0;
		};

		struct Analysis
		{
			std::vector<Merged> timed;   // scopes and waits, by total descending
			std::vector<Merged> values;  // counters and gauges, by name
			std::size_t         frameRecords = 0;
			std::size_t         gaps = 0;
			double              gapMs = 0;
			Distribution        frameMs;
			Distribution        osfMs;
			double              osfShareMean = 0;
			double              osfShareP95 = 0;
			double              windowSeconds = 0;
		};

		double TicksToNs(double a_ticks, std::uint64_t a_frequency) { return a_ticks * 1e9 / static_cast<double>(a_frequency); }
		double TicksToUs(double a_ticks, std::uint64_t a_frequency) { return a_ticks * 1e6 / static_cast<double>(a_frequency); }
		double TicksToMs(double a_ticks, std::uint64_t a_frequency) { return a_ticks * 1e3 / static_cast<double>(a_frequency); }

		Distribution Distribute(std::vector<double> a_values)
		{
			Distribution out;
			out.count = a_values.size();
			if (a_values.empty()) {
				return out;
			}
			std::sort(a_values.begin(), a_values.end());
			double sum = 0;
			for (const double value : a_values) {
				sum += value;
			}
			out.mean = sum / static_cast<double>(a_values.size());
			out.p50 = NearestRank(a_values, 50);
			out.p95 = NearestRank(a_values, 95);
			out.p99 = NearestRank(a_values, 99);
			out.max = a_values.back();
			return out;
		}

		std::string_view KindName(Kind a_kind)
		{
			switch (a_kind) {
			case Kind::kWait:
				return "wait";
			case Kind::kCounter:
				return "counter";
			case Kind::kGauge:
				return "gauge";
			default:
				return "scope";
			}
		}

		Analysis Analyze(const Snapshot& a_snapshot)
		{
			Analysis                                      out;
			std::unordered_map<std::string, std::size_t> index;
			std::vector<Merged>                           merged;
			for (const auto& thread : a_snapshot.threads) {
				for (const auto& agg : thread.aggs) {
					if (agg.id >= a_snapshot.descs.size()) {
						continue;
					}
					const DescInfo& desc = a_snapshot.descs[agg.id];
					std::string     key = desc.name;
					key.push_back('\x1f');
					key.append(KindName(desc.kind));
					auto [it, inserted] = index.try_emplace(std::move(key), merged.size());
					if (inserted) {
						merged.emplace_back();
						merged.back().name = desc.name;
						merged.back().category = desc.category;
						merged.back().kind = desc.kind;
					}
					Merged& m = merged[it->second];
					m.count += agg.count;
					m.total += agg.total;
					m.min = std::min(m.min, agg.min);
					m.max = std::max(m.max, agg.max);
					if (agg.lastAt >= m.lastAt) {
						m.lastAt = agg.lastAt;
						m.last = agg.last;
					}
					m.allocs += agg.allocs;
					m.allocBytes += agg.allocBytes;
					for (std::uint32_t b = 0; b < kHistogramBuckets; ++b) {
						m.histogram[b] += agg.histogram[b];
					}
					if (std::find(m.sites.begin(), m.sites.end(), desc.site) == m.sites.end()) {
						m.sites.push_back(desc.site);
					}
					m.threads.push_back({ thread.tid, agg.count, agg.total, agg.max });
				}
			}
			for (auto& m : merged) {
				std::sort(m.threads.begin(), m.threads.end(), [](const auto& a, const auto& b) { return a.total > b.total; });
				(m.kind == Kind::kScope || m.kind == Kind::kWait ? out.timed : out.values).push_back(std::move(m));
			}
			std::sort(out.timed.begin(), out.timed.end(), [](const Merged& a, const Merged& b) { return a.total > b.total; });
			std::sort(out.values.begin(), out.values.end(), [](const Merged& a, const Merged& b) { return a.name < b.name; });

			const auto           freq = a_snapshot.frequency;
			std::vector<double> frameMs;
			std::vector<double> osfMs;
			std::vector<double> share;
			out.frameRecords = a_snapshot.frames.size();
			for (const auto& frame : a_snapshot.frames) {
				const double ms = TicksToMs(static_cast<double>(frame.ticks), freq);
				if (ms > kGapFrameMs) {
					++out.gaps;
					out.gapMs += ms;
					continue;
				}
				frameMs.push_back(ms);
				if (frame.osfFinal) {
					const double osf = TicksToMs(static_cast<double>(frame.osfTicks), freq);
					osfMs.push_back(osf);
					share.push_back(ms > 0 ? osf / ms : 0.0);
				}
			}
			out.frameMs = Distribute(std::move(frameMs));
			out.osfMs = Distribute(std::move(osfMs));
			const Distribution shareDist = Distribute(std::move(share));
			out.osfShareMean = shareDist.mean;
			out.osfShareP95 = shareDist.p95;
			out.windowSeconds = static_cast<double>(a_snapshot.endTicks - a_snapshot.startTicks) / static_cast<double>(freq);
			return out;
		}

		std::string SanitizeLabel(std::string_view a_label)
		{
			std::string out;
			for (const char c : a_label) {
				if (out.size() >= 64) {
					break;
				}
				const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
				out.push_back(keep ? c : '-');
			}
			while (!out.empty() && (out.front() == '-' || out.front() == '.')) {
				out.erase(out.begin());
			}
			return out.empty() ? std::string("capture") : out;
		}

		std::string LocalStamp()
		{
			const std::time_t now = std::time(nullptr);
			std::tm           local{};
			localtime_s(&local, &now);
			char buffer[32]{};
			std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &local);
			return buffer;
		}

		void AppendEscaped(std::string& a_out, std::string_view a_text)
		{
			for (const char c : a_text) {
				switch (c) {
				case '"':
					a_out += "\\\"";
					break;
				case '\\':
					a_out += "\\\\";
					break;
				case '\n':
					a_out += "\\n";
					break;
				case '\r':
					a_out += "\\r";
					break;
				case '\t':
					a_out += "\\t";
					break;
				default:
					if (static_cast<unsigned char>(c) < 0x20) {
						std::format_to(std::back_inserter(a_out), "\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
					} else {
						a_out.push_back(c);
					}
				}
			}
		}

		// Streams text through zlib (gzip framing) into a file.
		class GzipWriter
		{
		public:
			explicit GzipWriter(const std::filesystem::path& a_path)
			{
				if (_wfopen_s(&_file, a_path.c_str(), L"wb") != 0) {
					_file = nullptr;
				}
				if (!_file) {
					return;
				}
				_ok = deflateInit2(&_stream, Z_BEST_SPEED, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK;
				_initialized = _ok;
				_buffer.reserve(kChunk + 4096);
			}
			~GzipWriter()
			{
				if (_initialized) {
					deflateEnd(&_stream);
				}
				if (_file) {
					std::fclose(_file);
				}
			}
			GzipWriter(const GzipWriter&) = delete;
			GzipWriter& operator=(const GzipWriter&) = delete;

			std::string& Buffer() { return _buffer; }

			void MaybeFlush()
			{
				if (_buffer.size() >= kChunk) {
					Deflate(Z_NO_FLUSH);
				}
			}

			bool Finish()
			{
				Deflate(Z_FINISH);
				if (_file && std::fflush(_file) != 0) {
					_ok = false;
				}
				return _ok;
			}

		private:
			static constexpr std::size_t kChunk = 1u << 20;

			void Deflate(int a_flush)
			{
				if (!_ok) {
					_buffer.clear();
					return;
				}
				_stream.next_in = reinterpret_cast<Bytef*>(_buffer.data());
				_stream.avail_in = static_cast<uInt>(_buffer.size());
				unsigned char out[1u << 16];
				int           status = Z_OK;
				do {
					_stream.next_out = out;
					_stream.avail_out = sizeof(out);
					status = deflate(&_stream, a_flush);
					if (status == Z_STREAM_ERROR) {
						_ok = false;
						break;
					}
					const std::size_t produced = sizeof(out) - _stream.avail_out;
					if (produced && std::fwrite(out, 1, produced, _file) != produced) {
						_ok = false;
						break;
					}
				} while (_stream.avail_out == 0 || (a_flush == Z_FINISH && status != Z_STREAM_END));
				_buffer.clear();
			}

			std::FILE*  _file = nullptr;
			z_stream    _stream{};
			std::string _buffer;
			bool        _ok = false;
			bool        _initialized = false;
		};

		bool Promote(const std::filesystem::path& a_partial, const std::filesystem::path& a_final)
		{
			return MoveFileExW(a_partial.c_str(), a_final.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
		}

		bool WriteTrace(const Snapshot& a_snapshot, const DumpMeta& a_meta, const std::filesystem::path& a_path)
		{
			const auto freq = a_snapshot.frequency;
			const auto start = a_snapshot.startTicks;
			const auto us = [&](std::uint64_t a_ticks) {
				return TicksToUs(static_cast<double>(a_ticks >= start ? a_ticks - start : 0), freq);
			};

			std::vector<const EventCopy*> events;
			events.reserve(a_snapshot.events.size());
			for (const auto& event : a_snapshot.events) {
				if (event.id < a_snapshot.descs.size()) {
					events.push_back(&event);
				}
			}
			std::stable_sort(events.begin(), events.end(), [](const EventCopy* a, const EventCopy* b) { return a->begin < b->begin; });

			// Bin timed events into frames by begin time.
			struct FrameBin
			{
				std::uint64_t calls = 0;
				std::uint64_t osfTicks = 0;
				bool          spill = false;
			};
			const auto&           frames = a_snapshot.frames;
			std::vector<FrameBin> bins(frames.size());
			for (const EventCopy* event : events) {
				if (event->kind != Kind::kScope && event->kind != Kind::kWait) {
					continue;
				}
				auto it = std::upper_bound(frames.begin(), frames.end(), event->begin,
					[](std::uint64_t a_at, const FrameCopy& a_frame) { return a_at < a_frame.begin; });
				if (it == frames.begin()) {
					continue;
				}
				--it;
				if (event->begin >= it->begin + it->ticks) {
					continue;
				}
				FrameBin& bin = bins[static_cast<std::size_t>(it - frames.begin())];
				if (event->kind == Kind::kScope) {
					++bin.calls;
				}
				if (event->depth == 0) {
					bin.osfTicks += event->end - event->begin;
					bin.spill = bin.spill || event->end > it->begin + it->ticks;
				}
			}

			GzipWriter   gz{ a_path };
			std::string& out = gz.Buffer();
			out += "{\"displayTimeUnit\":\"ms\",\"otherData\":{";
			std::format_to(std::back_inserter(out), "\"schema\":1,\"plugin\":\"");
			AppendEscaped(out, a_meta.plugin);
			out += "\",\"build\":\"";
			out += kProfilingBuildMarker;
			out += "\",\"dllVersion\":\"";
			AppendEscaped(out, a_meta.dllVersion);
			out += "\",\"gameVersion\":\"";
			AppendEscaped(out, a_meta.gameVersion);
			std::format_to(std::back_inserter(out), "\",\"mode\":\"{}\",\"label\":\"", ModeName(a_snapshot.mode));
			AppendEscaped(out, a_meta.label);
			out += "\",\"reason\":\"";
			AppendEscaped(out, a_meta.reason);
			out += "\",\"captureStart\":\"";
			AppendEscaped(out, a_snapshot.startedAt);
			std::format_to(std::back_inserter(out),
				"\",\"qpcFrequency\":{},\"captureStartTicks\":{},\"captureStartMonotonicNs\":{},\"ringCapacity\":{},\"threads\":{{",
				freq, start, a_snapshot.startMonotonicNs, a_snapshot.ringCapacity);
			bool firstThread = true;
			for (const auto& thread : a_snapshot.threads) {
				std::format_to(std::back_inserter(out), "{}\"{}\":{{\"events\":{},\"torn\":{},\"wrapped\":{}}}",
					firstThread ? "" : ",", thread.tid, thread.events, thread.torn, thread.wrapped ? "true" : "false");
				firstThread = false;
			}
			out += "}},\"traceEvents\":[\n";

			// Metadata: process, the Frames pseudo-thread, then every instrumented thread (main first).
			out += "{\"ph\":\"M\",\"pid\":1,\"tid\":0,\"name\":\"process_name\",\"args\":{\"name\":\"Starfield / ";
			AppendEscaped(out, a_meta.plugin);
			out += "\"}},\n{\"ph\":\"M\",\"pid\":1,\"tid\":0,\"name\":\"thread_name\",\"args\":{\"name\":\"Frames\"}},\n"
				   "{\"ph\":\"M\",\"pid\":1,\"tid\":0,\"name\":\"thread_sort_index\",\"args\":{\"sort_index\":-2}}";
			for (const auto& thread : a_snapshot.threads) {
				std::format_to(std::back_inserter(out), ",\n{{\"ph\":\"M\",\"pid\":1,\"tid\":{},\"name\":\"thread_name\",\"args\":{{\"name\":\"", thread.tid);
				AppendEscaped(out, thread.isMain ? std::string("Game Main (") + thread.name + ")" : thread.name);
				std::format_to(std::back_inserter(out), " [{}]\"}}}},\n{{\"ph\":\"M\",\"pid\":1,\"tid\":{},\"name\":\"thread_sort_index\",\"args\":{{\"sort_index\":{}}}}}",
					thread.osTid, thread.tid, thread.isMain ? -1 : static_cast<int>(thread.tid));
			}

			for (std::size_t i = 0; i < frames.size(); ++i) {
				const FrameCopy& frame = frames[i];
				const double     ms = TicksToMs(static_cast<double>(frame.ticks), freq);
				std::format_to(std::back_inserter(out),
					",\n{{\"ph\":\"X\",\"pid\":1,\"tid\":0,\"ts\":{:.3f},\"dur\":{:.3f},\"name\":\"Frame #{}\",\"cat\":\"Frame\","
					"\"args\":{{\"osfUs\":{:.1f},\"calls\":{},\"spill\":{},\"gap\":{}}}}}",
					us(frame.begin), TicksToUs(static_cast<double>(frame.ticks), freq), frame.index,
					TicksToUs(static_cast<double>(bins[i].osfTicks), freq), bins[i].calls,
					bins[i].spill ? "true" : "false", ms > kGapFrameMs ? "true" : "false");
				gz.MaybeFlush();
			}

			std::unordered_map<std::uint32_t, std::int64_t> runningCounters;
			for (const EventCopy* event : events) {
				const DescInfo& desc = a_snapshot.descs[event->id];
				switch (event->kind) {
				case Kind::kScope:
				case Kind::kWait:
					std::format_to(std::back_inserter(out), ",\n{{\"ph\":\"X\",\"pid\":1,\"tid\":{},\"ts\":{:.3f},\"dur\":{:.3f},\"name\":\"",
						event->tid, us(event->begin), TicksToUs(static_cast<double>(event->end - event->begin), freq));
					AppendEscaped(out, desc.name);
					out += "\",\"cat\":\"";
					AppendEscaped(out, event->kind == Kind::kWait ? std::string_view("Lock") : std::string_view(desc.category));
					std::format_to(std::back_inserter(out), "\",\"args\":{{\"depth\":{}", event->depth);
					if (event->allocs) {
						std::format_to(std::back_inserter(out), ",\"allocs\":{},\"allocBytes\":{}", event->allocs, event->allocBytes);
					}
					out += "}}";
					break;
				case Kind::kCounter:
				case Kind::kGauge:
					{
						auto value = static_cast<std::int64_t>(event->end);
						if (event->kind == Kind::kCounter) {
							value = runningCounters[event->id] += value;
						}
						std::format_to(std::back_inserter(out), ",\n{{\"ph\":\"C\",\"pid\":1,\"tid\":{},\"ts\":{:.3f},\"name\":\"", event->tid, us(event->begin));
						AppendEscaped(out, desc.name);
						std::format_to(std::back_inserter(out), "\",\"args\":{{\"{}\":{}}}}}", event->kind == Kind::kCounter ? "total" : "value", value);
						break;
					}
				}
				gz.MaybeFlush();
			}

			for (const auto& mark : a_snapshot.marks) {
				std::format_to(std::back_inserter(out), ",\n{{\"ph\":\"i\",\"s\":\"g\",\"pid\":1,\"tid\":{},\"ts\":{:.3f},\"cat\":\"Mark\",\"name\":\"mark: ",
					mark.tid, us(mark.at));
				AppendEscaped(out, mark.label);
				out += "\"}";
			}
			out += "\n]}\n";
			return gz.Finish();
		}

		nlohmann::ordered_json DistributionJson(const Distribution& a_dist)
		{
			return { { "mean", a_dist.mean }, { "p50", a_dist.p50 }, { "p95", a_dist.p95 }, { "p99", a_dist.p99 }, { "max", a_dist.max } };
		}

		nlohmann::ordered_json StatsJson(const Snapshot& a_snapshot, const Analysis& a_analysis, const DumpMeta& a_meta)
		{
			using json = nlohmann::ordered_json;
			const auto freq = a_snapshot.frequency;
			const auto ns = [&](double a_ticks) { return TicksToNs(a_ticks, freq); };

			json root;
			root["schema"] = 1;
			root["plugin"] = a_meta.plugin;
			root["build"] = kProfilingBuildMarker;
			root["dllVersion"] = a_meta.dllVersion;
			root["gameVersion"] = a_meta.gameVersion;
			root["mode"] = ModeName(a_snapshot.mode);
			root["label"] = a_meta.label;
			std::uint64_t torn = 0;
			bool          wrapped = false;
			for (const auto& thread : a_snapshot.threads) {
				torn += thread.torn;
				wrapped = wrapped || thread.wrapped;
			}
			root["capture"] = {
				{ "startedAt", a_snapshot.startedAt },
				{ "endedAt", a_snapshot.endedAt },
				{ "durationMs", a_analysis.windowSeconds * 1000.0 },
				{ "reason", a_meta.reason },
				{ "qpcFrequency", freq },
				{ "startMonotonicNs", a_snapshot.startMonotonicNs },
				{ "ringCapacity", a_snapshot.ringCapacity },
				{ "ringWrapped", wrapped },
				{ "tornEvents", torn },
				{ "droppedDescs", a_snapshot.droppedDescs },
				{ "droppedThreads", a_snapshot.droppedThreads },
			};
			root["frames"] = {
				{ "count", a_analysis.frameMs.count },
				{ "records", a_analysis.frameRecords },
				{ "gaps", a_analysis.gaps },
				{ "gapMs", a_analysis.gapMs },
				{ "frameMs", DistributionJson(a_analysis.frameMs) },
				{ "osfMsPerFrame", DistributionJson(a_analysis.osfMs) },
				{ "osfShareOfFrame", { { "mean", a_analysis.osfShareMean }, { "p95", a_analysis.osfShareP95 } } },
			};

			const double frameDivisor = static_cast<double>(std::max<std::size_t>(a_analysis.frameRecords, 1));
			json         scopes = json::array();
			for (const auto& m : a_analysis.timed) {
				json histogram = json::array();
				for (std::uint32_t b = 0; b < kHistogramBuckets; ++b) {
					if (m.histogram[b]) {
						histogram.push_back({ { "minNs", b == 0 ? 0.0 : ns(std::ldexp(1.0, static_cast<int>(b) - 1)) }, { "count", m.histogram[b] } });
					}
				}
				json threads = json::array();
				for (const auto& t : m.threads) {
					threads.push_back({ { "tid", t.tid }, { "count", t.count }, { "totalNs", ns(static_cast<double>(t.total)) }, { "maxNs", ns(static_cast<double>(t.max)) } });
				}
				scopes.push_back({
					{ "name", m.name },
					{ "category", m.category },
					{ "kind", KindName(m.kind) },
					{ "count", m.count },
					{ "totalNs", ns(static_cast<double>(m.total)) },
					{ "meanNs", m.count ? ns(static_cast<double>(m.total)) / static_cast<double>(m.count) : 0.0 },
					{ "minNs", m.count ? ns(static_cast<double>(m.min)) : 0.0 },
					{ "maxNs", m.count ? ns(static_cast<double>(m.max)) : 0.0 },
					{ "callsPerFrame", a_analysis.frameRecords ? static_cast<double>(m.count) / frameDivisor : 0.0 },
					{ "allocs", m.allocs },
					{ "allocBytes", m.allocBytes },
					{ "allocsPerCall", m.count ? static_cast<double>(m.allocs) / static_cast<double>(m.count) : 0.0 },
					{ "histogram", std::move(histogram) },
					{ "threads", std::move(threads) },
					{ "sites", m.sites },
				});
			}
			root["scopes"] = std::move(scopes);

			json counters = json::array();
			for (const auto& m : a_analysis.values) {
				if (m.kind == Kind::kCounter) {
					counters.push_back({ { "name", m.name }, { "kind", "counter" }, { "total", m.total }, { "increments", m.count }, { "sites", m.sites } });
				} else {
					counters.push_back({ { "name", m.name }, { "kind", "gauge" }, { "samples", m.count }, { "min", m.min }, { "max", m.max },
						{ "mean", m.count ? static_cast<double>(m.total) / static_cast<double>(m.count) : 0.0 }, { "last", m.last }, { "sites", m.sites } });
				}
			}
			root["counters"] = std::move(counters);

			json threads = json::array();
			for (const auto& thread : a_snapshot.threads) {
				threads.push_back({
					{ "tid", thread.tid },
					{ "osTid", thread.osTid },
					{ "name", thread.name },
					{ "isMain", thread.isMain },
					{ "exited", thread.exited },
					{ "current", thread.current },
					{ "events", thread.events },
					{ "tornEvents", thread.torn },
					{ "ringWrapped", thread.wrapped },
				});
			}
			root["threads"] = std::move(threads);

			std::vector<const Merged*> byAllocs;
			for (const auto& m : a_analysis.timed) {
				if (m.allocs) {
					byAllocs.push_back(&m);
				}
			}
			std::sort(byAllocs.begin(), byAllocs.end(), [](const Merged* a, const Merged* b) { return a->allocs > b->allocs; });
			json hot = json::array();
			for (std::size_t i = 0; i < byAllocs.size() && i < 20; ++i) {
				const Merged& m = *byAllocs[i];
				hot.push_back({ { "name", m.name }, { "allocs", m.allocs }, { "allocBytes", m.allocBytes },
					{ "allocsPerCall", static_cast<double>(m.allocs) / static_cast<double>(std::max<std::uint64_t>(m.count, 1)) } });
			}
			// Nested scopes include their children's allocations, so the hot list is not additive.
			root["allocations"] = { { "hotScopes", std::move(hot) } };
			return root;
		}

		std::vector<std::string> Summarize(const Snapshot& a_snapshot, const Analysis& a_analysis, const DumpMeta& a_meta)
		{
			const auto               freq = a_snapshot.frequency;
			std::vector<std::string> lines;
			lines.push_back(std::format("dump '{}' ({}, {}): window {:.1f} s, {} frames ({} gaps, {:.0f} ms)",
				a_meta.label, ModeName(a_snapshot.mode), a_meta.reason, a_analysis.windowSeconds,
				a_analysis.frameMs.count, a_analysis.gaps, a_analysis.gapMs));
			lines.push_back(std::format("frame ms p50 {:.2f} p95 {:.2f} p99 {:.2f} max {:.2f}; OSF ms/frame p50 {:.3f} p95 {:.3f} p99 {:.3f} max {:.3f}",
				a_analysis.frameMs.p50, a_analysis.frameMs.p95, a_analysis.frameMs.p99, a_analysis.frameMs.max,
				a_analysis.osfMs.p50, a_analysis.osfMs.p95, a_analysis.osfMs.p99, a_analysis.osfMs.max));

			const auto describe = [&](const Merged& m) {
				return std::format("{} {:.2f} ms ({}x, avg {:.1f} us, max {:.1f} us)", m.name,
					TicksToMs(static_cast<double>(m.total), freq), m.count,
					m.count ? TicksToUs(static_cast<double>(m.total), freq) / static_cast<double>(m.count) : 0.0,
					TicksToUs(static_cast<double>(m.max), freq));
			};
			std::string byTotal = "top by total:";
			for (std::size_t i = 0; i < a_analysis.timed.size() && i < 10; ++i) {
				byTotal += (i ? " | " : " ") + describe(a_analysis.timed[i]);
			}
			lines.push_back(a_analysis.timed.empty() ? "top by total: (no scopes recorded)" : byTotal);

			std::vector<const Merged*> byMax;
			for (const auto& m : a_analysis.timed) {
				byMax.push_back(&m);
			}
			std::sort(byMax.begin(), byMax.end(), [](const Merged* a, const Merged* b) { return a->max > b->max; });
			std::string maxLine = "top by max:";
			for (std::size_t i = 0; i < byMax.size() && i < 10; ++i) {
				maxLine += (i ? " | " : " ") + describe(*byMax[i]);
			}
			lines.push_back(byMax.empty() ? "top by max: (no scopes recorded)" : maxLine);

			std::vector<const Merged*> byAllocs;
			for (const auto& m : a_analysis.timed) {
				if (m.allocs) {
					byAllocs.push_back(&m);
				}
			}
			std::sort(byAllocs.begin(), byAllocs.end(), [](const Merged* a, const Merged* b) { return a->allocs > b->allocs; });
			std::string allocLine = "alloc hot spots:";
			for (std::size_t i = 0; i < byAllocs.size() && i < 5; ++i) {
				const Merged& m = *byAllocs[i];
				allocLine += std::format("{}{} {} allocs ({:.1f}/call, {} bytes)", i ? " | " : " ", m.name, m.allocs,
					static_cast<double>(m.allocs) / static_cast<double>(std::max<std::uint64_t>(m.count, 1)), m.allocBytes);
			}
			lines.push_back(byAllocs.empty() ? "alloc hot spots: none" : allocLine);
			return lines;
		}
	}

	DumpResult Write(const Snapshot& a_snapshot, const std::filesystem::path& a_dir, const DumpMeta& a_meta)
	{
		DumpResult result;
		try {
			DumpMeta meta = a_meta;
			meta.label = SanitizeLabel(a_meta.label);
			const Analysis analysis = Analyze(a_snapshot);
			result.summary = Summarize(a_snapshot, analysis, meta);

			std::filesystem::create_directories(a_dir);
			const std::string stamp = LocalStamp();
			std::string       base = std::format("{}-{}", stamp, meta.label);
			for (int suffix = 2; std::filesystem::exists(a_dir / (base + ".stats.json")) && suffix < 1000; ++suffix) {
				base = std::format("{}-{}-{}", stamp, meta.label, suffix);
			}

			const auto stats = a_dir / (base + ".stats.json");
			{
				const auto partial = a_dir / (base + ".stats.json.partial");
				const std::string text = StatsJson(a_snapshot, analysis, meta).dump(1, '\t');
				std::FILE*        file = nullptr;
				if (_wfopen_s(&file, partial.c_str(), L"wb") != 0) {
					file = nullptr;
				}
				bool ok = file && std::fwrite(text.data(), 1, text.size(), file) == text.size();
				if (file) {
					ok = std::fclose(file) == 0 && ok;
				}
				if (!ok || !Promote(partial, stats)) {
					std::error_code ignored;
					std::filesystem::remove(partial, ignored);
					result.error = std::format("could not write {}", stats.string());
				} else {
					result.stats = stats;
				}
			}

			if (a_snapshot.mode == Mode::kTrace || !a_snapshot.events.empty()) {
				const auto trace = a_dir / (base + ".trace.json.gz");
				const auto partial = a_dir / (base + ".trace.json.gz.partial");
				const bool ok = WriteTrace(a_snapshot, meta, partial);
				if (!ok || !Promote(partial, trace)) {
					std::error_code ignored;
					std::filesystem::remove(partial, ignored);
					result.error = std::format("could not write {}", trace.string());
				} else {
					result.trace = trace;
				}
			}
		} catch (const std::exception& e) {
			result.error = e.what();
		}
		if (!result.stats.empty()) {
			result.summary.push_back("wrote " + result.stats.string());
		}
		if (!result.trace.empty()) {
			result.summary.push_back("wrote " + result.trace.string());
		}
		return result;
	}
}
