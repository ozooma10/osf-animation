#include "Animation/IdlePlayer.h"

#include "Util/FormRef.h"

namespace OSF::Animation
{
	namespace
	{
		constexpr const char* kTemplateRefs[] = {
			"OSF.esm|0x800", "OSF.esm|0x801", "OSF.esm|0x802", "OSF.esm|0x803"
		};
		constexpr const char* kNonPlayerArchetypeRef = "Starfield.esm|0x000B6F0B";  // AnimArchetypeMacho (player gate-lift)
		constexpr const char* kPlayerArchetypeRef = "Starfield.esm|0x0006B508";     // AnimArchetypePlayer (player restore)
		constexpr float       kPlayerSettleSec = 0.10f;  // beat after the archetype swap before PlayIdle
		constexpr float       kPlayerMaxSec = 30.0f;     // safety archetype-restore timeout (a save while swapped is a footgun)
	}

	IdlePlayer& IdlePlayer::GetSingleton()
	{
		static IdlePlayer instance;
		return instance;
	}

	void IdlePlayer::Resolve()
	{
		_pool.clear();
		_nonPlayerKw = nullptr;
		_playerKw = nullptr;

		for (const char* ref : kTemplateRefs) {
			if (auto* idle = Util::ResolveFormRef<RE::TESIdleForm>(ref)) {
				_pool.push_back(idle);
			} else {
				REX::WARN("IdlePlayer: idle template '{}' did not resolve (is OSF.esm enabled?) — skipped", ref);
			}
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
			kPlayerSettleSec * 1000.0f, kPlayerMaxSec);
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
			if (!isPlayer) {
				idle->animFileName = RE::BSFixedString{ path.c_str() };  // GNAM +0x70, refcount-correct
				RE::ActorUtils::PlayIdle(a_actor, idle);
				REX::INFO("IdlePlayer: playing '{}' on actor {:X}", path, a_actor->formID);
				return;
			}

			// Player: lift the AnimArchetypePlayer gate, then defer PlayIdle a beat so the re-archetyped graph settles (firing it immediately is accepted but plays nothing).
			// ChangeAnimArchetype returns false even on success — judge by effect, ignore the return.
			RE::ActorUtils::ChangeAnimArchetype(a_actor, _nonPlayerKw);
			const auto now = Clock::now();
			{
				std::scoped_lock l{ _lock };
				_pendingPlayerPlay = PendingPlay{ idle, path, now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(kPlayerSettleSec)), false };
				_playerSwapped = true;
				_playerRestoreAt = (kPlayerMaxSec > 0.0f)
					? now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(kPlayerMaxSec))
					: Clock::time_point::max();
				_active.store(true, std::memory_order_relaxed);
			}
			REX::INFO("IdlePlayer: player archetype swapped; '{}' queued (settle {:.0f}ms)", path, kPlayerSettleSec * 1000.0f);
		});
		return true;
	}

	bool IdlePlayer::Stop(RE::Actor* a_actor)
	{
		if (!a_actor || a_actor != RE::PlayerCharacter::GetSingleton()) {
			return false;  // NPC idles have no archetype swap to undo; they end on their own
		}

		SFSE::GetTaskInterface()->AddTask([this]() {
			RE::BGSKeyword* restoreKw = nullptr;
			{
				std::scoped_lock l{ _lock };
				if (!_playerSwapped) {
					return;
				}
				restoreKw = _playerKw;
				_playerSwapped = false;
				_pendingPlayerPlay.reset();
				_active.store(false, std::memory_order_relaxed);
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (restoreKw && player) {
				RE::ActorUtils::ChangeAnimArchetype(player, restoreKw);
				REX::INFO("IdlePlayer: player archetype restored (Stop)");
			}
		});
		return true;
	}

	void IdlePlayer::Tick()
	{
		if (!_active.load(std::memory_order_relaxed)) {
			return;  // no player play in flight — free for the common (NPC-only / idle) case
		}

		const auto now = Clock::now();
		RE::TESIdleForm* toPlay = nullptr;
		std::string      toPath;
		bool             doRestore = false;
		RE::BGSKeyword*  restoreKw = nullptr;
		{
			std::scoped_lock l{ _lock };
			if (_playerSwapped && now >= _playerRestoreAt) {
				doRestore = true;
				restoreKw = _playerKw;
				_playerSwapped = false;
				_pendingPlayerPlay.reset();
			} else if (_pendingPlayerPlay && !_pendingPlayerPlay->fired && now >= _pendingPlayerPlay->fireAt) {
				toPlay = _pendingPlayerPlay->idle;
				toPath = _pendingPlayerPlay->path;
				_pendingPlayerPlay->fired = true;
			}
			_active.store(_playerSwapped || (_pendingPlayerPlay && !_pendingPlayerPlay->fired), std::memory_order_relaxed);
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		if (toPlay) {
			SFSE::GetTaskInterface()->AddTask([toPlay, toPath, player]() {
				toPlay->animFileName = RE::BSFixedString{ toPath.c_str() };  // re-assert GNAM right before play
				RE::ActorUtils::PlayIdle(player, toPlay);
				REX::INFO("IdlePlayer: player now playing '{}'", toPath);
			});
		}
		if (doRestore && restoreKw) {
			SFSE::GetTaskInterface()->AddTask([player, restoreKw]() {
				RE::ActorUtils::ChangeAnimArchetype(player, restoreKw);
				REX::INFO("IdlePlayer: player archetype restored (timeout)");
			});
		}
	}

	void IdlePlayer::OnStopAll()
	{
		// A world-replacing load is authority; drop our tracking. The archetype lives in the (rebuilt) anim graph, so there is nothing for us to restore here.
		std::scoped_lock l{ _lock };
		_pendingPlayerPlay.reset();
		_playerSwapped = false;
		_active.store(false, std::memory_order_relaxed);
	}
}
