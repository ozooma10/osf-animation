// Development-only global allocation counting for the profiling build and osf-profiler-test.
//
// Replaces every global operator new/delete in this module with the exact mapping the dynamic
// (/MD) CRT uses: malloc/free, and _aligned_malloc/_aligned_free for over-aligned types. Memory
// therefore stays interchangeable with other modules sharing ucrtbase. The only addition is three
// constant-initialized, trivially destructible thread_local counters that ScopeGuard samples.

#include "Profiling/Profiler.h"

#include <cstdlib>
#include <malloc.h>
#include <new>

namespace OSF::Profiling::detail
{
	thread_local std::uint64_t t_allocCount = 0;
	thread_local std::uint64_t t_allocBytes = 0;
	thread_local std::uint64_t t_freeCount = 0;
}

namespace
{
	using OSF::Profiling::detail::t_allocBytes;
	using OSF::Profiling::detail::t_allocCount;
	using OSF::Profiling::detail::t_freeCount;

	// nullptr only when allocation failed and no new-handler is installed; a handler may throw.
	void* Allocate(std::size_t a_size)
	{
		const std::size_t size = a_size ? a_size : 1;
		for (;;) {
			if (void* block = std::malloc(size)) {
				++t_allocCount;
				t_allocBytes += a_size;
				return block;
			}
			const std::new_handler handler = std::get_new_handler();
			if (!handler) {
				return nullptr;
			}
			handler();
		}
	}

	void* AllocateAligned(std::size_t a_size, std::align_val_t a_alignment)
	{
		const std::size_t size = a_size ? a_size : 1;
		for (;;) {
			if (void* block = _aligned_malloc(size, static_cast<std::size_t>(a_alignment))) {
				++t_allocCount;
				t_allocBytes += a_size;
				return block;
			}
			const std::new_handler handler = std::get_new_handler();
			if (!handler) {
				return nullptr;
			}
			handler();
		}
	}

	void Release(void* a_block) noexcept
	{
		if (a_block) {
			++t_freeCount;
			std::free(a_block);
		}
	}

	void ReleaseAligned(void* a_block) noexcept
	{
		if (a_block) {
			++t_freeCount;
			_aligned_free(a_block);
		}
	}

	void* OrThrow(void* a_block)
	{
		if (!a_block) {
			throw std::bad_alloc();
		}
		return a_block;
	}

	template <class F>
	void* NoThrow(F&& a_allocate) noexcept
	{
		try {
			return a_allocate();
		} catch (...) {
			return nullptr;
		}
	}
}

void* operator new(std::size_t a_size) { return OrThrow(Allocate(a_size)); }
void* operator new[](std::size_t a_size) { return OrThrow(Allocate(a_size)); }
void* operator new(std::size_t a_size, const std::nothrow_t&) noexcept { return NoThrow([&] { return Allocate(a_size); }); }
void* operator new[](std::size_t a_size, const std::nothrow_t&) noexcept { return NoThrow([&] { return Allocate(a_size); }); }
void* operator new(std::size_t a_size, std::align_val_t a_alignment) { return OrThrow(AllocateAligned(a_size, a_alignment)); }
void* operator new[](std::size_t a_size, std::align_val_t a_alignment) { return OrThrow(AllocateAligned(a_size, a_alignment)); }
void* operator new(std::size_t a_size, std::align_val_t a_alignment, const std::nothrow_t&) noexcept
{
	return NoThrow([&] { return AllocateAligned(a_size, a_alignment); });
}
void* operator new[](std::size_t a_size, std::align_val_t a_alignment, const std::nothrow_t&) noexcept
{
	return NoThrow([&] { return AllocateAligned(a_size, a_alignment); });
}

void operator delete(void* a_block) noexcept { Release(a_block); }
void operator delete[](void* a_block) noexcept { Release(a_block); }
void operator delete(void* a_block, std::size_t) noexcept { Release(a_block); }
void operator delete[](void* a_block, std::size_t) noexcept { Release(a_block); }
void operator delete(void* a_block, const std::nothrow_t&) noexcept { Release(a_block); }
void operator delete[](void* a_block, const std::nothrow_t&) noexcept { Release(a_block); }
void operator delete(void* a_block, std::align_val_t) noexcept { ReleaseAligned(a_block); }
void operator delete[](void* a_block, std::align_val_t) noexcept { ReleaseAligned(a_block); }
void operator delete(void* a_block, std::size_t, std::align_val_t) noexcept { ReleaseAligned(a_block); }
void operator delete[](void* a_block, std::size_t, std::align_val_t) noexcept { ReleaseAligned(a_block); }
void operator delete(void* a_block, std::align_val_t, const std::nothrow_t&) noexcept { ReleaseAligned(a_block); }
void operator delete[](void* a_block, std::align_val_t, const std::nothrow_t&) noexcept { ReleaseAligned(a_block); }
