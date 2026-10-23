#pragma once

#include "Input/InputTypes.h"
#include <array>
#include <atomic>
#include <string_view>

namespace OSF::Input
{
	// Settings mod IDs, one per shipped schema. Alignment lives on its own page so the main
	// OSF Animation page stays short for players who never author placement.
	inline constexpr const char* kEngineModId = "osf.animation.engine";
	inline constexpr const char* kAlignModId = "osf.animation.align";

	// `mod`/`setting` are null for a fixed secondary binding that no schema exposes.
	struct KeyDefinition { const char* mod; const char* setting; Verb verb; std::uint32_t key; };
	inline constexpr auto kSceneKeys = std::to_array<KeyDefinition>({
		{ kEngineModId, "keys.advance", Verb::kAdvance, 0x20 },
		{ kEngineModId, "keys.pause", Verb::kPause, 0x50 },
		{ kEngineModId, "keys.speedUp", Verb::kSpeedUp, 0x6B },
		{ kEngineModId, "keys.speedDown", Verb::kSpeedDown, 0x6D },
		{ kEngineModId, "keys.speedReset", Verb::kSpeedReset, 0x30 },
		{ kEngineModId, "keys.end", Verb::kEnd, 0x23 },
		{ kAlignModId, "keys.alignSelect", Verb::kAlignSelect, 0x65 },
		{ kAlignModId, "keys.alignStep", Verb::kAlignStep, 0x6F },
		{ kAlignModId, "keys.alignXPlus", Verb::kAlignXPlus, 0x66 },
		{ kAlignModId, "keys.alignXMinus", Verb::kAlignXMinus, 0x64 },
		{ kAlignModId, "keys.alignYPlus", Verb::kAlignYPlus, 0x68 },
		{ kAlignModId, "keys.alignYMinus", Verb::kAlignYMinus, 0x62 },
		{ kAlignModId, "keys.alignZPlus", Verb::kAlignZPlus, 0x63 },
		{ kAlignModId, "keys.alignZMinus", Verb::kAlignZMinus, 0x61 },
		{ kAlignModId, "keys.alignHeadingPlus", Verb::kAlignHeadingPlus, 0x69 },
		{ kAlignModId, "keys.alignHeadingMinus", Verb::kAlignHeadingMinus, 0x67 },
		{ kAlignModId, "keys.alignReset", Verb::kAlignReset, 0x6E },
		{ kAlignModId, "keys.alignSave", Verb::kAlignSave, 0x6A },
		// Top-row +/- aliases for the numpad speed keys, documented in the speed hints. Last, so
		// Lookup resolves any configurable binding rebound onto these keys ahead of the alias.
		{ nullptr, nullptr, Verb::kSpeedUp, 0xBB },
		{ nullptr, nullptr, Verb::kSpeedDown, 0xBD }
	});

	inline constexpr bool Configurable(const KeyDefinition& a_key) { return a_key.setting != nullptr; }

	// True when this definition is owned by a_mod's schema.
	inline bool OwnedBy(const KeyDefinition& a_key, std::string_view a_mod)
	{
		return a_key.mod != nullptr && a_mod == a_key.mod;
	}

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
