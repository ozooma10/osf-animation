#include "API/NativeSceneEventRegistry.h"

#include <iostream>
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
}

int main()
{
	TestFilterMaskAndUnregister();
	TestSelfUnregisterAndGeneration();
	TestZeroMaskAndExceptionIsolation();
	if (failures != 0) {
		std::cerr << failures << " native scene-event test(s) failed\n";
		return 1;
	}
	std::cout << "Native scene-event tests passed\n";
	return 0;
}
