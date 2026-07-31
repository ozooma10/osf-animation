#include "API/NativeSceneEventRegistry.h"

#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	int failures = 0;

#define CHECK(expr)                                                            \
	do {                                                                        \
		if (!(expr)) {                                                          \
			std::cerr << __FILE__ << ':' << __LINE__                            \
			          << ": CHECK failed: " #expr << '\n';                     \
			++failures;                                                         \
		}                                                                       \
	} while (false)

	struct Recorder
	{
		std::vector<std::string> calls;
	};

	void Record(const OSF::API::OSFSceneEvent* a_event, void* a_context)
	{
		auto& recorder = *static_cast<Recorder*>(a_context);
		recorder.calls.emplace_back(a_event->cue);
		CHECK(a_event->size == sizeof(OSF::API::OSFSceneEvent));
	}

	struct SelfRemoving
	{
		OSF::API::NativeSceneEventRegistry* registry;
		std::uint64_t token = 0;
		int calls = 0;
	};

	void RemoveSelf(const OSF::API::OSFSceneEvent*, void* a_context)
	{
		auto& self = *static_cast<SelfRemoving*>(a_context);
		++self.calls;
		CHECK(self.registry->Unregister(self.token));
	}

	void Throw(const OSF::API::OSFSceneEvent*, void*)
	{
		throw std::runtime_error("consumer failure");
	}

	void Increment(const OSF::API::OSFSceneEvent*, void* a_context)
	{
		++*static_cast<int*>(a_context);
	}

	struct BlockingCallback
	{
		std::mutex lock;
		std::condition_variable cv;
		bool entered = false;
		bool release = false;
	};

	void Block(const OSF::API::OSFSceneEvent*, void* a_context)
	{
		auto& state = *static_cast<BlockingCallback*>(a_context);
		std::unique_lock lock{ state.lock };
		state.entered = true;
		state.cv.notify_all();
		state.cv.wait(lock, [&] { return state.release; });
	}

	struct SelfRemovingWithPeer
	{
		OSF::API::NativeSceneEventRegistry* registry = nullptr;
		std::uint64_t token = 0;
		std::mutex lock;
		std::condition_variable cv;
		int entered = 0;
		bool unregisterStarted = false;
		bool releasePeer = false;
		bool unregisterReturned = false;
		bool unregisterResult = false;
	};

	void RemoveSelfWhilePeerRuns(const OSF::API::OSFSceneEvent*, void* a_context)
	{
		auto& state = *static_cast<SelfRemovingWithPeer*>(a_context);
		std::unique_lock lock{ state.lock };
		const int ordinal = ++state.entered;
		state.cv.notify_all();
		if (ordinal == 1) {
			state.cv.wait(lock, [&] { return state.entered == 2; });
			state.unregisterStarted = true;
			state.cv.notify_all();
			lock.unlock();
			const bool result = state.registry->Unregister(state.token);
			lock.lock();
			state.unregisterResult = result;
			state.unregisterReturned = true;
			state.cv.notify_all();
		} else {
			state.cv.wait(lock, [&] { return state.releasePeer; });
		}
	}

	void TestFilterMaskAndUnregister()
	{
		using namespace OSF::API;
		NativeSceneEventRegistry registry;
		Recorder recorder;
		CHECK(registry.Register(nullptr, nullptr, 0, SceneEventType::kAll) == 0);

		const auto token = registry.Register(
			&Record, &recorder, 42, SceneEventType::kCue);
		CHECK(token != 0);

		OSFSceneEvent event;
		event.sceneHandle = 41;
		event.eventType = SceneEventType::kCue;
		event.cue = "wrong-scene";
		CHECK(registry.Dispatch(event) == 0);
		CHECK(recorder.calls.empty());

		event.sceneHandle = 42;
		event.eventType = SceneEventType::kAction;
		event.cue = "wrong-mask";
		CHECK(registry.Dispatch(event) == 0);
		CHECK(recorder.calls.empty());

		event.eventType = SceneEventType::kCue;
		event.cue = "suitprotocol.stow.suspend";
		CHECK(registry.Dispatch(event) == 0);
		CHECK(recorder.calls ==
		      std::vector<std::string>{ "suitprotocol.stow.suspend" });

		CHECK(registry.Unregister(token));
		CHECK(!registry.Unregister(token));
		CHECK(registry.Dispatch(event) == 0);
		CHECK(recorder.calls.size() == 1);
	}

	void TestSelfUnregisterAndGeneration()
	{
		using namespace OSF::API;
		NativeSceneEventRegistry registry;
		SelfRemoving self{ .registry = &registry };
		self.token = registry.Register(
			&RemoveSelf, &self, 0, SceneEventType::kCue);
		CHECK(self.token != 0);

		OSFSceneEvent event;
		event.eventType = SceneEventType::kCue;
		CHECK(registry.Dispatch(event) == 0);
		CHECK(self.calls == 1);
		CHECK(registry.Dispatch(event) == 0);
		CHECK(self.calls == 1);

		int replacementCalls = 0;
		const auto replacement = registry.Register(
			&Increment, &replacementCalls, 0, SceneEventType::kCue);
		CHECK(replacement != 0);
		CHECK(replacement != self.token);
		CHECK(!registry.Unregister(self.token));
		CHECK(registry.Dispatch(event) == 0);
		CHECK(replacementCalls == 1);
	}

	void TestZeroMaskAndExceptionIsolation()
	{
		using namespace OSF::API;
		NativeSceneEventRegistry registry;
		int calls = 0;
		CHECK(registry.Register(&Throw, nullptr, 0, 0) != 0);
		CHECK(registry.Register(&Increment, &calls, 0, 0) != 0);

		OSFSceneEvent event;
		event.eventType = SceneEventType::kSceneEnd;
		CHECK(registry.Dispatch(event) == 1);
		CHECK(calls == 1);
	}

	void TestConcurrentUnregisterWaitsForCallback()
	{
		using namespace OSF::API;
		using namespace std::chrono_literals;

		NativeSceneEventRegistry registry;
		BlockingCallback state;
		const auto token = registry.Register(
			&Block, &state, 0, SceneEventType::kCue);
		CHECK(token != 0);

		OSFSceneEvent event;
		event.eventType = SceneEventType::kCue;
		std::size_t dispatchFailures = 1;
		std::thread dispatchThread([&] {
			dispatchFailures = registry.Dispatch(event);
		});

		{
			std::unique_lock lock{ state.lock };
			state.cv.wait(lock, [&] { return state.entered; });
		}

		std::promise<void> unregisterStarted;
		auto started = unregisterStarted.get_future();
		auto unregister = std::async(std::launch::async, [&] {
			unregisterStarted.set_value();
			return registry.Unregister(token);
		});
		started.wait();
		CHECK(unregister.wait_for(50ms) == std::future_status::timeout);

		{
			std::lock_guard lock{ state.lock };
			state.release = true;
		}
		state.cv.notify_all();
		dispatchThread.join();
		CHECK(dispatchFailures == 0);
		CHECK(unregister.get());
		CHECK(registry.Dispatch(event) == 0);
	}

	void TestSelfUnregisterWaitsForConcurrentPeer()
	{
		using namespace OSF::API;
		using namespace std::chrono_literals;

		NativeSceneEventRegistry registry;
		SelfRemovingWithPeer state{ .registry = &registry };
		state.token = registry.Register(
			&RemoveSelfWhilePeerRuns, &state, 0, SceneEventType::kCue);
		CHECK(state.token != 0);

		OSFSceneEvent event;
		event.eventType = SceneEventType::kCue;
		std::size_t firstFailures = 1;
		std::size_t secondFailures = 1;
		std::thread first([&] { firstFailures = registry.Dispatch(event); });
		{
			std::unique_lock lock{ state.lock };
			state.cv.wait(lock, [&] { return state.entered == 1; });
		}
		std::thread second([&] { secondFailures = registry.Dispatch(event); });

		{
			std::unique_lock lock{ state.lock };
			state.cv.wait(lock, [&] { return state.unregisterStarted; });
			CHECK(!state.cv.wait_for(lock, 50ms, [&] { return state.unregisterReturned; }));
			state.releasePeer = true;
		}
		state.cv.notify_all();
		first.join();
		second.join();
		CHECK(firstFailures == 0);
		CHECK(secondFailures == 0);
		CHECK(state.unregisterReturned);
		CHECK(state.unregisterResult);
		CHECK(registry.Dispatch(event) == 0);
	}
}

int main()
{
	TestFilterMaskAndUnregister();
	TestSelfUnregisterAndGeneration();
	TestZeroMaskAndExceptionIsolation();
	TestConcurrentUnregisterWaitsForCallback();
	TestSelfUnregisterWaitsForConcurrentPeer();
	if (failures != 0) {
		std::cerr << failures << " native scene-event test(s) failed\n";
		return 1;
	}
	std::cout << "Native scene-event tests passed\n";
	return 0;
}