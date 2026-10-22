// Compiled WITHOUT OSF_PROFILING: proves the production expansion of every profiling macro has no
// side effects and that the lock macro is an ordinary lock declaration.

#include "Profiling/Profile.h"

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <type_traits>

#if defined(OSF_PROFILING) && OSF_PROFILING
#	error ProfilerNoopTest.cpp must be compiled without OSF_PROFILING
#endif

namespace OSF::Test::ProfilerNoop
{
	namespace
	{
		// True when another thread cannot take a_mutex exclusively right now.
		template <class M>
		bool HeldElsewhere(M& a_mutex)
		{
			bool acquired = true;
			std::thread probe([&] {
				acquired = a_mutex.try_lock();
				if (acquired) {
					a_mutex.unlock();
				}
			});
			probe.join();
			return !acquired;
		}
	}

	int SideEffects()
	{
		int effects = 0;
		OSF_PROFILE_SCOPE("Noop.Scope");
		OSF_PROFILE_COUNTER("Noop.Counter", ++effects);
		const int called = OSF_PROFILE_CALL("Noop.Call", effects + 40);  // the one macro that evaluates its expression
		if (called != 40) {
			return -1;
		}
		OSF_PROFILE_GAUGE("Noop.Gauge", ++effects);
		OSF_PROFILE_THREAD_NAME("noop");
		OSF_PROFILE_FRAME();
		return effects;
	}

	bool TaskIsIdentity()
	{
		const int first = 1;
		const int second = 2;
		auto      fn = [first, second] { return first + second; };
		auto      wrapped = OSF_PROFILE_TASK("Noop.Task", [first, second] { return first + second; });
		static_assert(std::is_same_v<decltype(OSF_PROFILE_TASK("Noop.Task", fn)), decltype((fn))>);
		return wrapped() == 3 && fn() == 3;
	}

	bool LocksReallyLock()
	{
		std::mutex        plain;
		std::mutex        second;
		std::shared_mutex shared;
		bool              ok = true;
		{
			OSF_PROFILE_LOCK(std::unique_lock, lock, "Noop.Lock.unique", plain);
			ok = ok && lock.owns_lock() && HeldElsewhere(plain);
		}
		{
			OSF_PROFILE_LOCK(std::shared_lock, lock, "Noop.Lock.shared", shared);
			ok = ok && lock.owns_lock() && HeldElsewhere(shared);
		}
		{
			OSF_PROFILE_LOCK(std::lock_guard, lock, "Noop.Lock.guard", plain);
			ok = ok && HeldElsewhere(plain);
		}
		{
			OSF_PROFILE_LOCK(std::scoped_lock, lock, "Noop.Lock.scoped", plain, second);
			ok = ok && HeldElsewhere(plain) && HeldElsewhere(second);
		}
		return ok && !HeldElsewhere(plain) && !HeldElsewhere(second) && !HeldElsewhere(shared);
	}
}
