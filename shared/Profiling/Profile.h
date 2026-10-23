#pragma once

// Development-only profiler instrumentation (docs/PROFILING.md).
//
// Only the "OSF Animation" profiling override build (`xmake f --profiling=y`) and osf-profiler-test
// define OSF_PROFILING=1. Everywhere else every macro below is a no-op or its plain C++ equivalent,
// so the shipped DLL carries no timer reads, profiler branches or profiler state.
//
// Arguments are never evaluated in production: never pass an expression with side effects.
// Names are string literals of the form "Category.Sub.Leaf"; the category is the text before the
// first '.'. Lock waits use "Lock.<owner>.<mutex>.<shared|unique>". Scopes never log.

#if defined(OSF_PROFILING) && OSF_PROFILING

#include "Profiling/Profiler.h"

#define OSF_PP_CAT_(a_left, a_right) a_left##a_right
#define OSF_PP_CAT(a_left, a_right) OSF_PP_CAT_(a_left, a_right)

// Expression-context descriptor: one constant-initialized static per expansion site.
#define OSF_PROFILE_DESC(a_name)                                                         \
	([]() -> ::OSF::Profiling::ScopeDesc& {                                              \
		static ::OSF::Profiling::ScopeDesc osfProfDesc{ a_name, __FILE__, __LINE__ };    \
		return osfProfDesc;                                                              \
	}())

// Times the enclosing block on the calling thread.
#define OSF_PROFILE_SCOPE(a_name)                                                                       \
	static ::OSF::Profiling::ScopeDesc OSF_PP_CAT(osfProfDesc_, __LINE__){ a_name, __FILE__, __LINE__ }; \
	const ::OSF::Profiling::ScopeGuard OSF_PP_CAT(osfProfScope_, __LINE__){ OSF_PP_CAT(osfProfDesc_, __LINE__) }

// Times one expression and yields its value: `auto x = OSF_PROFILE_CALL("A.B", Load(path));`.
// Production expands to the parenthesized expression, so the shipped code is unchanged.
#define OSF_PROFILE_CALL(a_name, ...)              \
	([&]() -> decltype(auto) {                     \
		OSF_PROFILE_SCOPE(a_name);                 \
		return __VA_ARGS__;                        \
	}())

// Adds a_delta to a global running total (Chrome "C" event in trace mode).
#define OSF_PROFILE_COUNTER(a_name, a_delta) \
	::OSF::Profiling::Counter(OSF_PROFILE_DESC(a_name), static_cast<std::int64_t>(a_delta))

// Samples a per-thread value (min/max/last; Chrome "C" event in trace mode).
#define OSF_PROFILE_GAUGE(a_name, a_value) \
	::OSF::Profiling::Gauge(OSF_PROFILE_DESC(a_name), static_cast<std::int64_t>(a_value))

// Declares `Type var{ mutexes... }` and records how long the construction (the lock wait) took.
// The name precedes the mutex list so std::scoped_lock with several mutexes works.
#define OSF_PROFILE_LOCK(Type, a_var, a_name, ...)                                                          \
	const std::uint64_t OSF_PP_CAT(osfLockBegin_, __LINE__) = ::OSF::Profiling::Now();                      \
	Type                a_var{ __VA_ARGS__ };                                                               \
	::OSF::Profiling::RecordInterval(OSF_PROFILE_DESC(a_name), OSF_PP_CAT(osfLockBegin_, __LINE__),         \
		::OSF::Profiling::Now())

// Wraps a callable so a named scope opens whenever it runs (queued tasks, audio jobs). Variadic
// because a lambda's capture list ("[a, b]") is not protected from macro argument splitting.
#define OSF_PROFILE_TASK(a_name, ...) ::OSF::Profiling::Tagged(OSF_PROFILE_DESC(a_name), __VA_ARGS__)

#define OSF_PROFILE_THREAD_NAME(a_name) ::OSF::Profiling::NameCurrentThread(a_name)

// Once per game frame, on the frame-maintenance thread only.
#define OSF_PROFILE_FRAME() ::OSF::Profiling::FrameMark()

#else

#define OSF_PROFILE_SCOPE(a_name) ((void)0)
#define OSF_PROFILE_CALL(a_name, ...) (__VA_ARGS__)
#define OSF_PROFILE_COUNTER(a_name, a_delta) ((void)0)
#define OSF_PROFILE_GAUGE(a_name, a_value) ((void)0)
#define OSF_PROFILE_LOCK(Type, a_var, a_name, ...) Type a_var{ __VA_ARGS__ }
#define OSF_PROFILE_TASK(a_name, ...) (__VA_ARGS__)
#define OSF_PROFILE_THREAD_NAME(a_name) ((void)0)
#define OSF_PROFILE_FRAME() ((void)0)

#endif
