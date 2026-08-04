#include "Audio/SoundPlayback.h"

#include "Audio/SoundService.h"
#include "Matchmaking/Matchmaker.h"
#include "Registry/SoundRegistry.h"
#include "UI/Subtitle.h"

namespace OSF::Audio::SoundPlayback
{
	void Play(RE::Actor* a_actor, std::uint64_t a_fallbackChannel, std::string_view a_spec,
		Registry::SoundEmitter a_emitter, std::string_view a_diagnosticOwner)
	{
		std::string spec(a_spec);
		if (!spec.empty() && spec.front() == '$') {
			const std::string gender = a_actor ? Matchmaking::ActorGenderTag(a_actor) : std::string{};
			for (std::size_t p = 0; (p = spec.find("{gender}", p)) != std::string::npos; p += gender.size()) {
				spec.replace(p, 8, gender);
			}
			auto resolved = Registry::SoundRegistry::GetSingleton().Resolve(spec);
			if (!resolved) {
				REX::DEBUG("[Sound] {} pool '{}' matched no clip — skipped", a_diagnosticOwner, spec);
				return;
			}
			spec = std::move(*resolved);
		}

		const std::uint64_t slot = a_actor ?
			((1ull << 62) | static_cast<std::uint64_t>(a_actor->formID)) :
			((2ull << 62) | a_fallbackChannel);
		auto& service = SoundService::GetSingleton();
		std::uint64_t gameObject = 0;
		if (a_emitter == Registry::SoundEmitter::kRole) {
			gameObject = service.PrepareRoleEmitter(a_actor);
		}
		REX::DEBUG("[Sound] {} playing '{}' (emitter {}, slot {:#x})", a_diagnosticOwner, spec,
			gameObject ? "role" : "listener", slot);
		service.Play(slot, spec, gameObject);
		const std::string text = Registry::SoundRegistry::GetSingleton().TextForClip(spec);
		if (!text.empty()) UI::Subtitle::Show(a_actor, text, 0.0f);
	}
}
