#pragma once

#include "Registry/SceneRegistry.h"

#include <cstdint>
#include <string_view>

namespace RE { class Actor; }

namespace OSF::Audio::SoundPlayback
{
	// Shared scene/overlay sound-policy path: pool resolution, {gender}, channel selection,
	// emitter preparation, and subtitles. a_fallbackChannel is used only without an actor.
	void Play(RE::Actor* a_actor, std::uint64_t a_fallbackChannel, std::string_view a_spec,
		Registry::SoundEmitter a_emitter, std::string_view a_diagnosticOwner);
}
