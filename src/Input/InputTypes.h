#pragma once

// Shared input vocabulary: capabilities a scene grants, the verbs the input layer routes, and the per-scene Grant. 
// Kept dependency-free (types only) so both the scene registry (parse) and the scene runtime can use it without depending on the InputService.

namespace RE
{
	class Actor;
}

namespace OSF::Input
{
	// Capability groups a scene's `playerControl.allow` can grant. Bitmask on the scene def + the active Grant.
	enum class Capability : std::uint32_t
	{
		kAdvance = 1u << 0,     // take the node's default advance edge
		kNavigate = 1u << 1,    // branch edges
		kSpeed = 1u << 2,       // faster / slower / reset / pause the shared scene clock
		kReposition = 1u << 3,  // nudge / rotate / realign actors (not implemented yet)
		kFreecam = 1u << 4,     // free camera (not implemented yet)
		kEnd = 1u << 5,         // end the scene (subject to playerControl.locked)
	};

	// Map an `allow` string to its capability bit; 0 if unrecognized (caller warns).
	inline std::uint32_t CapabilityBit(std::string_view a_name)
	{
		if (a_name == "advance") return static_cast<std::uint32_t>(Capability::kAdvance);
		if (a_name == "navigate") return static_cast<std::uint32_t>(Capability::kNavigate);
		if (a_name == "speed") return static_cast<std::uint32_t>(Capability::kSpeed);
		if (a_name == "reposition") return static_cast<std::uint32_t>(Capability::kReposition);
		if (a_name == "freecam") return static_cast<std::uint32_t>(Capability::kFreecam);
		if (a_name == "end") return static_cast<std::uint32_t>(Capability::kEnd);
		return 0;
	}

	// Fine-grained director verbs the input layer routes to runtime actions. v1 keyboard set;
	// navigate/reposition/freecam verbs are deferred. Each verb requires its capability granted.
	enum class Verb : std::uint32_t
	{
		kNone = 0,
		kAdvance,     // -> SceneRuntime::Advance         (cap kAdvance)
		kSpeedUp,     // -> faster                        (cap kSpeed)
		kSpeedDown,   // -> slower                        (cap kSpeed)
		kSpeedReset,  // -> 1.0                           (cap kSpeed)
		kPause,       // -> freeze / resume               (cap kSpeed)
		kEnd,         // -> SceneRuntime::Stop            (cap kEnd, blocked if Grant.locked)
	};

	// The capability a verb requires (0 for kNone).
	inline std::uint32_t RequiredCapability(Verb a_verb)
	{
		switch (a_verb) {
		case Verb::kAdvance:
			return static_cast<std::uint32_t>(Capability::kAdvance);
		case Verb::kSpeedUp:
		case Verb::kSpeedDown:
		case Verb::kSpeedReset:
		case Verb::kPause:
			return static_cast<std::uint32_t>(Capability::kSpeed);
		case Verb::kEnd:
			return static_cast<std::uint32_t>(Capability::kEnd);
		default:
			return 0;
		}
	}

	// The per-scene control grant the runtime hands the InputService when a playerControl scene starts. 
	// `driver` is the participant whose scene the local input drives (the player basically).
	struct Grant
	{
		std::int32_t  handle = 0;
		std::uint32_t caps = 0;        // OR of Capability bits the scene granted
		RE::Actor*    driver = nullptr;
		bool          locked = false;  // player may not End
	};
}
