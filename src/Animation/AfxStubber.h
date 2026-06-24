#pragma once

// A loose custom `.af` (played via the engine's NT_DYNAMIC_ANIMATION / IDLE-GNAM path) needs a
// `.afx` annotation sidecar on disk — without one the engine rejects the idle (PlayIdle returns
// false). The `.afx` is read during the startup AnimTextData scan, NOT per-play, so a sidecar
// written lazily would only take effect after a restart.
//
// EnsureAfxStubs() writes a minimal stub `.afx` next to every loose `.af` missing one, so authors
// ship only `.af`. It MUST run before the AnimTextData scan, so call it from SFSE_PLUGIN_LOAD
// (earliest hook, before the game's resource/data init). The stub is generic — the engine loads the
// `.af` by its GNAM path regardless of the `.afx`'s internal <filename> — so one template fits all.

namespace OSF::Animation
{
	// Scan the loose Data/meshes/actors tree and write a stub `.afx` for every `.af` lacking one.
	// Idempotent (never overwrites an author-shipped `.afx`); safe to call every launch.
	void EnsureAfxStubs();
}
