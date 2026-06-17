#pragma once

// Delivers scene-runtime events to registered Papyrus callbacks.
//
// There's no synchronous C++->Papyrus path: the only entries available
// (DispatchStaticCall / DispatchMethodCall) queue a Papyrus stack on the VM scheduler and
// return immediately, so the receiver runs later, off this stack. That rules out a
// "dispatch-time getter" model, so instead we snapshot the payload into a `Var[]` argument
// (receiver signature `Function MyFn(Var[] akEvent)`, decoded via OSFEvent.psc). The async
// dispatch also gives us, for free, the guarantee that callbacks aren't reentrant and any
// mutations they make land later.

namespace OSF::Scene
{
	// Event-type bits — mirror the OSF.psc EVENT_* constants.
	namespace Event
	{
		inline constexpr std::int32_t kNodeEnter = 0x01;
		inline constexpr std::int32_t kNodeExit = 0x02;
		inline constexpr std::int32_t kCue = 0x04;
		inline constexpr std::int32_t kAction = 0x08;
		inline constexpr std::int32_t kActionFailed = 0x10;
		inline constexpr std::int32_t kSceneEnd = 0x20;
		inline constexpr std::int32_t kSceneAbort = 0x40;
		inline constexpr std::int32_t kAll = 0xFFFF;
	}

	// One scene event, snapshotted and dispatched to registered Papyrus callbacks as a
	// `Var[]` payload. Index layout is frozen with the ABI (see OSFEvent.psc + Pack()).
	// Fields irrelevant to a given event keep their defaults.
	struct SceneEvent
	{
		std::int32_t scene = 0;        // [0] scene handle
		std::int32_t event = 0;        // [1] EVENT_*
		std::string  node;             // [2] node id
		std::string  edge;             // [3] edge id
		std::string  cue;              // [4] EVENT_CUE id
		std::string  actionType;       // [5] EVENT_ACTION / EVENT_ACTION_FAILED type
		RE::Actor*   actor = nullptr;  // [6] participant (may be null)
		std::string  role;             // [7] role name
		std::int32_t loopIndex = -1;   // [8]
		float        time = -1.0f;     // [9] clip-local fraction, or -1.0 for a named anchor
		std::string  anchor;           // [10] "", "enter", "exit", "end"
		std::int32_t result = 0;       // [11] RESULT_*
	};

	// Token-based registry of Papyrus scene-event callbacks + the C++->Papyrus dispatch
	// path. Thread-safe; every receiver runs later on the VM scheduler.
	class SceneEventRelay
	{
	public:
		static SceneEventRelay& GetSingleton();

		// Register a_receiver.a_fn(Var[]) for events whose bit is set in a_eventMask and
		// (when a_sceneFilter != 0) whose scene == a_sceneFilter. Returns a generational
		// token (0 = failed: null receiver or no VM).
		std::int32_t Register(const RE::BSTSmartPointer<RE::BSScript::Object>& a_receiver, std::string_view a_fn,
			std::int32_t a_sceneFilter, std::int32_t a_eventMask);

		// Remove the registration for a_token. False if the token is stale/invalid.
		bool Unregister(std::int32_t a_token);

		// Snapshot a_event into a Var[] and DispatchMethodCall every matching receiver,
		// in registration order. Safe from any thread.
		void Dispatch(const SceneEvent& a_event);

		// DEBUG/no-instance transport probe: DispatchStaticCall a_script.a_fn(Var[]) with
		// a_event's payload (no registration needed). Proves the Var[] marshalling without
		// a scripted form. Returns false if the VM is unavailable.
		bool DispatchStatic(std::string_view a_script, std::string_view a_fn, const SceneEvent& a_event);

		// Drop every registration (load teardown).
		void Clear();

	private:
		struct Entry
		{
			std::uint16_t                             generation = 0;  // 0 = empty slot
			RE::BSTSmartPointer<RE::BSScript::Object> receiver;
			RE::BSFixedString                         fn;
			std::int32_t                              sceneFilter = 0;
			std::int32_t                              eventMask = 0;
		};

		// token = (generation << 16) | slot ; token 0 = null (slot 0 gen>=1 -> nonzero).
		static std::int32_t MakeToken(std::uint16_t a_gen, std::uint16_t a_slot)
		{
			return (static_cast<std::int32_t>(a_gen) << 16) | a_slot;
		}

		std::mutex         _lock;
		std::vector<Entry> _slots;
		std::uint16_t      _nextGen = 1;
	};
}
