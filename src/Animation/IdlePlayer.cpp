#include "Animation/IdlePlayer.h"

#include "Util/FormRef.h"

namespace OSF::Animation
{
	namespace
	{
		constexpr const char* kTemplateRefs[] = {
			"OSF.esm|0x800", "OSF.esm|0x801", "OSF.esm|0x802", "OSF.esm|0x803"
		};
		// OSF_IdleStop (ENAM=IdleStop). PlayIdle()d before a new clip to interrupt whatever idle is
		// running. "IdleStop" is a Graph-Event-class anim event, so the engine REJECTS it when pushed
		// externally via NotifyAnimationGraph — the idle subsystem (PlayIdle of this form) is the only
		// path that delivers it, exactly how AAF (Fallout 4) stops idles with its LooseIdleStop form.
		constexpr const char* kStopIdleRef = "OSF.esm|0x804";
		constexpr const char* kNonPlayerArchetypeRef = "Starfield.esm|0x000B6F0B";  // AnimArchetypeMacho (player gate-lift)
		constexpr const char* kPlayerArchetypeRef = "Starfield.esm|0x0006B508";     // AnimArchetypePlayer (player restore)
		constexpr float       kSettleSec = 0.20f;     // beat between the stop idle / archetype swap and PlayIdle (matches AAF)
		constexpr float       kPlayerMaxSec = 30.0f;  // safety archetype-restore timeout (a save while swapped is a footgun)

		std::chrono::steady_clock::duration Secs(float a_sec)
		{
			return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(a_sec));
		}
	}

	IdlePlayer& IdlePlayer::GetSingleton()
	{
		static IdlePlayer instance;
		return instance;
	}

	void IdlePlayer::Resolve()
	{
		_pool.clear();
		_stopIdle = nullptr;
		_nonPlayerKw = nullptr;
		_playerKw = nullptr;

		for (const char* ref : kTemplateRefs) {
			if (auto* idle = Util::ResolveFormRef<RE::TESIdleForm>(ref)) {
				_pool.push_back(idle);
			} else {
				REX::WARN("IdlePlayer: idle template '{}' did not resolve (is OSF.esm enabled?) — skipped", ref);
			}
		}

		_stopIdle = Util::ResolveFormRef<RE::TESIdleForm>(kStopIdleRef);
		if (!_stopIdle) {
			REX::WARN("IdlePlayer: stop idle '{}' did not resolve — a new play won't interrupt a running idle", kStopIdleRef);
		}

		_nonPlayerKw = Util::ResolveFormRef<RE::BGSKeyword>(kNonPlayerArchetypeRef);
		if (!_nonPlayerKw) {
			REX::WARN("IdlePlayer: AnimArchetypeMacho '{}' did not resolve — player playback disabled", kNonPlayerArchetypeRef);
		}
		_playerKw = Util::ResolveFormRef<RE::BGSKeyword>(kPlayerArchetypeRef);
		if (!_playerKw) {
			REX::WARN("IdlePlayer: AnimArchetypePlayer '{}' did not resolve — player playback disabled", kPlayerArchetypeRef);
		}

		_resolved.store(true, std::memory_order_release);

		const bool playerOk = _nonPlayerKw && _playerKw;
		REX::INFO("IdlePlayer: {} template(s) resolved; NPC playback {}, player playback {} (settle {:.0f}ms, timeout {:.0f}s)",
			_pool.size(),
			_pool.empty() ? "UNAVAILABLE (OSF.esm not enabled?)" : "ready",
			_pool.empty() ? "UNAVAILABLE (OSF.esm not enabled?)" : (playerOk ? "ready" : "disabled (archetype keyword missing)"),
			kSettleSec * 1000.0f, kPlayerMaxSec);
	}

	RE::TESIdleForm* IdlePlayer::NextTemplate()
	{
		if (_pool.empty()) {
			return nullptr;
		}
		const std::size_t i = _rr.fetch_add(1, std::memory_order_relaxed) % _pool.size();
		return _pool[i];
	}

	bool IdlePlayer::Play(RE::Actor* a_actor, std::string_view a_afPath)
	{
		if (!_resolved.load(std::memory_order_acquire) || _pool.empty()) {
			REX::WARN("IdlePlayer.Play: no idle templates resolved (is OSF.esm enabled?)");
			return false;
		}
		if (!a_actor) {
			REX::WARN("IdlePlayer.Play: no actor given");
			return false;
		}

		const bool isPlayer = (a_actor == RE::PlayerCharacter::GetSingleton());
		if (isPlayer && (!_nonPlayerKw || !_playerKw)) {
			REX::WARN("IdlePlayer.Play: player playback unavailable — an anim-archetype keyword did not resolve");
			return false;
		}

		RE::TESIdleForm* idle = NextTemplate();
		if (!idle) {
			return false;
		}
		std::string path{ a_afPath };

		// All TESIdleForm mutation + the engine idle/archetype calls run on the game thread.
		SFSE::GetTaskInterface()->AddTask([this, a_actor, idle, path, isPlayer]() {
			// Lift the player's archetype gate, interrupt any idle already playing (PlayIdle the stop
			// idle — the idle subsystem is the only path that delivers "IdleStop"), then defer the real
			// PlayIdle a settle beat (Tick fires it) so the interrupted idle clears / the graph settles.
			if (isPlayer) {
				// ChangeAnimArchetype returns false even on success — judge by effect, ignore the return.
				RE::ActorUtils::ChangeAnimArchetype(a_actor, _nonPlayerKw);
			}
			if (_stopIdle) {
				RE::ActorUtils::PlayIdle(a_actor, _stopIdle);
			}
			const auto now = Clock::now();
			{
				std::scoped_lock l{ _lock };
				_pending[a_actor] = PendingPlay{ idle, path, now + Secs(kSettleSec) };
				if (isPlayer) {
					_playerSwapped = true;
					_playerRestoreAt = (kPlayerMaxSec > 0.0f) ? now + Secs(kPlayerMaxSec) : Clock::time_point::max();
				}
				_active.store(true, std::memory_order_relaxed);
			}
			REX::INFO("IdlePlayer: '{}' queued on actor {:X} (settle {:.0f}ms{})",
				path, a_actor->formID, kSettleSec * 1000.0f, isPlayer ? ", player archetype swapped" : "");
		});
		return true;
	}

	bool IdlePlayer::Stop(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		const bool isPlayer = (a_actor == RE::PlayerCharacter::GetSingleton());

		SFSE::GetTaskInterface()->AddTask([this, a_actor, isPlayer]() {
			if (_stopIdle) {
				RE::ActorUtils::PlayIdle(a_actor, _stopIdle);  // end the running idle (NPC + player)
			}
			RE::BGSKeyword* restoreKw = nullptr;
			{
				std::scoped_lock l{ _lock };
				_pending.erase(a_actor);  // cancel a not-yet-fired play
				if (isPlayer && _playerSwapped) {
					restoreKw = _playerKw;
					_playerSwapped = false;
				}
				_active.store(!_pending.empty() || _playerSwapped, std::memory_order_relaxed);
			}
			if (restoreKw) {
				RE::ActorUtils::ChangeAnimArchetype(a_actor, restoreKw);  // ignore false-on-success return
				REX::INFO("IdlePlayer: player archetype restored (Stop)");
			}
		});
		return true;
	}

	void IdlePlayer::Tick()
	{
		if (!_active.load(std::memory_order_relaxed)) {
			return;  // nothing in flight — free for the common (idle) case
		}

		const auto now = Clock::now();
		struct Fire
		{
			RE::Actor*       actor;
			RE::TESIdleForm* idle;
			std::string      path;
		};
		std::vector<Fire> fired;
		bool              doRestore = false;
		RE::BGSKeyword*   restoreKw = nullptr;
		{
			std::scoped_lock l{ _lock };
			for (auto it = _pending.begin(); it != _pending.end();) {
				if (now >= it->second.fireAt) {
					fired.push_back({ it->first, it->second.idle, it->second.path });
					it = _pending.erase(it);
				} else {
					++it;
				}
			}
			if (_playerSwapped && now >= _playerRestoreAt) {
				doRestore = true;
				restoreKw = _playerKw;
				_playerSwapped = false;
			}
			_active.store(!_pending.empty() || _playerSwapped, std::memory_order_relaxed);
		}

		for (const auto& f : fired) {
			SFSE::GetTaskInterface()->AddTask([f]() {
				f.idle->animFileName = RE::BSFixedString{ f.path.c_str() };  // re-assert GNAM right before play
				RE::ActorUtils::PlayIdle(f.actor, f.idle);
				REX::INFO("IdlePlayer: now playing '{}' on actor {:X}", f.path, f.actor->formID);
			});
		}
		if (doRestore && restoreKw) {
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				SFSE::GetTaskInterface()->AddTask([player, restoreKw]() {
					RE::ActorUtils::ChangeAnimArchetype(player, restoreKw);
					REX::INFO("IdlePlayer: player archetype restored (timeout)");
				});
			}
		}
	}

	void IdlePlayer::OnStopAll()
	{
		// A world-replacing load is authority; drop our tracking. The archetype lives in the (rebuilt) anim graph, so there is nothing for us to restore here.
		std::scoped_lock l{ _lock };
		_pending.clear();
		_playerSwapped = false;
		_active.store(false, std::memory_order_relaxed);
	}
}
