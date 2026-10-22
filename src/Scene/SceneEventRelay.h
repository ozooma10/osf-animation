#pragma once

namespace OSF::Scene
{
	// Mirrors OSF.psc EVENT_* constants.
	namespace Event
	{
		inline constexpr std::int32_t kNodeEnter = 0x01;
		inline constexpr std::int32_t kNodeExit = 0x02;
		inline constexpr std::int32_t kCue = 0x04;
		inline constexpr std::int32_t kAction = 0x08;
		inline constexpr std::int32_t kSceneEnd = 0x10;
		inline constexpr std::int32_t kSceneBegin = 0x20;
		inline constexpr std::int32_t kNodeTransition = 0x40;
		inline constexpr std::int32_t kAll = 0xFFFF;
	}

	// Dispatched as OSFTypes:SceneEvent; Papyrus member names are frozen ABI.
	struct SceneEvent
	{
		std::int32_t scene = 0;
		std::int32_t event = 0;        // EVENT_*
		std::string  node;
		std::string  edge;
		std::string  cue;
		std::string  actionType;
		RE::Actor*   actor = nullptr;  // may be null
		std::string  role;
		std::int32_t loopIndex = -1;   // reserved
		float        time = -1.0f;     // clip-local fraction, -1 for a named track position
		std::string  trackPosition;    // "", "enter", "exit", "end"; Papyrus field `anchor`
		std::int32_t result = 0;       // RESULT_*
		std::int32_t cause = -1;       // SCENE_END only: completed=0, cancelled=1, interrupted=2
		std::uint64_t detectedNs{}; // native only; Papyrus payload is unchanged
		bool nativeOnly = false; // internal routing for opt-in cues; not a public payload field
	};

	// Thread-safe.
	class SceneEventRelay
	{
	public:
		static SceneEventRelay& GetSingleton();

		// a_sceneFilter 0 = any scene. Returns a generational token, 0 = failed.
		std::int32_t Register(const RE::BSTSmartPointer<RE::BSScript::Object>& a_receiver, std::string_view a_fn, std::int32_t a_sceneFilter, std::int32_t a_eventMask);

		// For global functions on script libraries (no receiver object).
		std::int32_t RegisterStatic(std::string_view a_script, std::string_view a_fn,
			std::int32_t a_sceneFilter, std::int32_t a_eventMask);

		bool Unregister(std::int32_t a_token);

		void Dispatch(const SceneEvent& a_event);

		void Clear();

	private:
		struct Entry
		{
			std::uint16_t                             generation = 0;  // 0 = empty slot
			RE::BSTSmartPointer<RE::BSScript::Object> receiver;
			RE::BSFixedString                         scriptName;      // set => static dispatch; receiver unused
			RE::BSFixedString                         fn;
			std::int32_t                              sceneFilter = 0;
			std::int32_t                              eventMask = 0;
		};

		// Caller holds _lock.
		std::int32_t AddEntry(const RE::BSTSmartPointer<RE::BSScript::Object>& a_receiver, RE::BSFixedString a_scriptName, std::string_view a_fn, std::int32_t a_sceneFilter, std::int32_t a_eventMask);

		// token = (generation << 16) | slot; generation >= 1 keeps tokens nonzero.
		static std::int32_t MakeToken(std::uint16_t a_gen, std::uint16_t a_slot)
		{
			return (static_cast<std::int32_t>(a_gen) << 16) | a_slot;
		}

		std::mutex         _lock;
		std::vector<Entry> _slots;
		std::uint16_t      _nextGen = 1;
	};
}
