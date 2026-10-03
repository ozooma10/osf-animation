#pragma once

// Types only, so the scene registry can use them without depending on InputService.

namespace RE
{
	class Actor;
}

namespace OSF::Input
{
	// Bits revocable via `sceneControls.disable` (legacy key `playerControl.disable`).
	enum class Capability : std::uint32_t
	{
		kAdvance = 1u << 0,
		kNavigate = 1u << 1,
		kSpeed = 1u << 2,
		// bit 3 reserved
		kFreecam = 1u << 4,
		kEnd = 1u << 5,
	};

	inline constexpr std::uint32_t kAllCapabilities =
		static_cast<std::uint32_t>(Capability::kAdvance) |
		static_cast<std::uint32_t>(Capability::kNavigate) |
		static_cast<std::uint32_t>(Capability::kSpeed) |
		static_cast<std::uint32_t>(Capability::kFreecam) |
		static_cast<std::uint32_t>(Capability::kEnd);

	// 0 if unrecognized (caller warns).
	inline std::uint32_t CapabilityBit(std::string_view a_name)
	{
		if (a_name == "advance") return static_cast<std::uint32_t>(Capability::kAdvance);
		if (a_name == "navigate") return static_cast<std::uint32_t>(Capability::kNavigate);
		if (a_name == "speed") return static_cast<std::uint32_t>(Capability::kSpeed);
		if (a_name == "freecam") return static_cast<std::uint32_t>(Capability::kFreecam);
		if (a_name == "end") return static_cast<std::uint32_t>(Capability::kEnd);
		return 0;
	}

	// Each verb requires its capability.
	enum class Verb : std::uint32_t
	{
		kNone = 0,
		kAdvance,
		kSpeedUp,
		kSpeedDown,
		kSpeedReset,
		kPause,
		kFreecam,
		kEnd,         // blocked if Grant.locked
	};


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
		case Verb::kFreecam:
			return static_cast<std::uint32_t>(Capability::kFreecam);
		case Verb::kEnd:
			return static_cast<std::uint32_t>(Capability::kEnd);
		default:
			return 0;
		}
	}

	// `driver` is the participant the local input drives (the player).
	struct Grant
	{
		std::int32_t  handle = 0;
		std::uint32_t capabilities = 0;
		RE::Actor*    driver = nullptr;
		bool          locked = false;  // player may not End
	};
}
