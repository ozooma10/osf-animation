#pragma once

#include "Input/InputTypes.h"
#include <array>
#include <atomic>

namespace OSF::Input
{
	struct KeyDefinition { const char* setting; Verb verb; std::uint32_t key; };
	inline constexpr auto kSceneKeys = std::to_array<KeyDefinition>({
		{ "keys.advance", Verb::kAdvance, 0x20 },
		{ "keys.speedUp", Verb::kSpeedUp, 0x6B }, { "keys.speedUpAlt", Verb::kSpeedUp, 0xBB },
		{ "keys.speedDown", Verb::kSpeedDown, 0x6D }, { "keys.speedDownAlt", Verb::kSpeedDown, 0xBD },
		{ "keys.speedReset", Verb::kSpeedReset, 0x30 }, { "keys.pause", Verb::kPause, 0x50 },
		{ "keys.end", Verb::kEnd, 0x23 },
		{ "keys.alignSelect", Verb::kAlignSelect, 0x65 },
		{ "keys.alignXPlus", Verb::kAlignXPlus, 0x66 }, { "keys.alignXMinus", Verb::kAlignXMinus, 0x64 },
		{ "keys.alignYPlus", Verb::kAlignYPlus, 0x68 }, { "keys.alignYMinus", Verb::kAlignYMinus, 0x62 },
		{ "keys.alignZPlus", Verb::kAlignZPlus, 0x63 }, { "keys.alignZMinus", Verb::kAlignZMinus, 0x61 },
		{ "keys.alignHeadingPlus", Verb::kAlignHeadingPlus, 0x69 }, { "keys.alignHeadingMinus", Verb::kAlignHeadingMinus, 0x67 },
		{ "keys.alignStep", Verb::kAlignStep, 0x6F }, { "keys.alignReset", Verb::kAlignReset, 0x6E },
		{ "keys.alignSave", Verb::kAlignSave, 0x6A }
	});

	class SceneKeymap
	{
	public:
		SceneKeymap() { for (std::size_t i = 0; i < kSceneKeys.size(); ++i) _keys[i] = kSceneKeys[i].key; }
		void Set(std::size_t index, std::uint32_t key) { if (index < _keys.size() && key <= 0xFF) _keys[index].store(key, std::memory_order_relaxed); }
		Verb Lookup(std::uint32_t key) const
		{
			if (key == 0xFF) return Verb::kNone; // OSF Settings unbound sentinel
			for (std::size_t i = 0; i < _keys.size(); ++i)
				if (_keys[i].load(std::memory_order_relaxed) == key) return kSceneKeys[i].verb;
			return Verb::kNone;
		}
	private:
		std::array<std::atomic<std::uint32_t>, kSceneKeys.size()> _keys;
	};
	inline SceneKeymap g_sceneKeymap;
}
