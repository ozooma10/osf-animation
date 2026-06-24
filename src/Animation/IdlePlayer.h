#pragma once

// `.af` playback via the engine's own animation graph (via NT_DYNAMIC_ANIMATION). 
// NPCs play it directly; the PLAYER is gated by its anim archetype (AnimArchetypePlayer rejects non-player clips), so we ChangeAnimArchetype off it,
// let the re-archetyped graph settle a beat, PlayIdle, then restore the player archetype on stop/timeout.

#include <chrono>

namespace RE
{
	class Actor;
	class BGSKeyword;
	class TESIdleForm;
}

namespace OSF::Animation
{
	class IdlePlayer
	{
	public:
		static IdlePlayer& GetSingleton();

		// Resolve the fixed template/keyword refs against the loaded plugins. Call once at kPostDataLoad (TESDataHandler must be up).
		void Resolve();

		// --- Playback (thread-safe entry points; all engine mutation lands on the game thread) ---
		// Play `a_afPath` on `a_actor`. `a_afPath` is the path as the engine expects it in an IDLE GNAM (under meshes\, no "meshes\" prefix, CamelCase filename), e.g. "Actors\Human\Animations\OSF\mydance.af". 
		// Returns false synchronously only on a hard reject (not configured / null actor / player without archetype keywords); the play itself is async.
		bool Play(RE::Actor* a_actor, std::string_view a_afPath);

		// Restore the player's anim archetype now (cancels a pending/settling player play). For NPCs there is no archetype to restore, so this is a no-op (returns false), the NPC idle ends on its own. 
		// Returns true if a player restore was queued.
		bool Stop(RE::Actor* a_actor);

		// Per-frame pulse from GraphManager's anim-graph hook (job thread). Drives the player's settle->PlayIdle step and the safety-timeout archetype restore. Cheap early-out when idle.
		void Tick();

		// Save-load safety: drop player-swap tracking (the reloaded world is authority). Called from GraphManager::StopAll. Does NOT itself restore the archetype (the graph is being rebuilt).
		void OnStopAll();

	private:
		IdlePlayer() = default;

		RE::TESIdleForm* NextTemplate();

		// resolved at kPostDataLoad (written once, read during gameplay) from the fixed refs in IdlePlayer.cpp
		std::vector<RE::TESIdleForm*> _pool;
		RE::BGSKeyword*               _nonPlayerKw = nullptr;
		RE::BGSKeyword*               _playerKw = nullptr;
		std::atomic<bool>             _resolved{ false };
		std::atomic<std::size_t>      _rr{ 0 };  // round-robin cursor over _pool

		// player archetype-swap lifecycle (only one player, so a single slot suffices)
		using Clock = std::chrono::steady_clock;
		std::mutex _lock;
		struct PendingPlay
		{
			RE::TESIdleForm* idle = nullptr;
			std::string      path;
			Clock::time_point fireAt{};
			bool             fired = false;
		};
		std::optional<PendingPlay> _pendingPlayerPlay;  // settle window before PlayIdle
		bool                       _playerSwapped = false;
		Clock::time_point          _playerRestoreAt{};
		std::atomic<bool>          _active{ false };  // cheap Tick early-out (player play in flight)
	};
}
