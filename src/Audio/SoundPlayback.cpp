#include "Audio/SoundPlayback.h"
#include "Profiling/Profile.h"

#include "Audio/SoundService.h"
#include "Testing/D3AudioProbe.h"
#include "Matchmaking/Matchmaker.h"
#include "Registry/SoundRegistry.h"
#include "UI/Subtitle.h"

#include <spdlog/spdlog.h>

namespace OSF::Audio::SoundPlayback
{
	void Play(RE::Actor* a_actor, std::uint64_t a_fallbackChannel, std::string_view a_spec,
		Registry::SoundEmitter a_emitter, std::string_view a_diagnosticOwner)
	{
		OSF_PROFILE_SCOPE("Audio.Play");
#ifdef OSF_ANIMATION_SMOKE
		if (Testing::D3AudioProbe::TryScopedSound(a_spec)) return;
#endif
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
		const bool roleRequested = a_emitter == Registry::SoundEmitter::kRole;
		if (roleRequested) {
			gameObject = service.PrepareRoleEmitter(a_actor);
			// An unavailable actor emitter must not turn a distant cue into loud centered audio.
			static std::atomic<bool> warnedFallback{ false };
			if (!gameObject && !warnedFallback.exchange(true, std::memory_order_relaxed)) {
				REX::WARN("[Sound] emitter 'role' requested by {} is unavailable — positioned cue skipped",
					a_diagnosticOwner);
			}
			if (!gameObject) return;
		}
		if (spdlog::should_log(spdlog::level::debug)) {
			REX::DEBUG("[Sound] {} playing '{}' (emitter {}, slot {:#x})", a_diagnosticOwner, spec,
				gameObject ? "role" : (roleRequested ? "listener (role fallback)" : "listener"), slot);
		}
		service.Play(slot, spec, gameObject);
		const std::string text = Registry::SoundRegistry::GetSingleton().TextForClip(spec);
		if (!text.empty()) UI::Subtitle::Show(a_actor, text, 0.0f);
	}
}
