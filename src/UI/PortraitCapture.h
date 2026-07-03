#pragma once

#include <filesystem>
#include <functional>

// The proven single-actor headshot capture (OSF RE module ui.menu3d_paperdoll,
// reference Probe/PortraitProbe.cpp, brief 2026-07-03). It HIJACKS the live engine
// inventory-paperdoll doll: with the paperdoll screen open, swap the doll's base to a
// target TESNPC, rebuild its face (Actor::UpdateAppearance), capture the composited
// frame to PNG (CreationRenderer::CaptureFrameToFile), then restore the player's face.
//
// Constraints that shape the API (all from the brief):
//  - MENU-BOUND: only works while the inventory paperdoll is open + rendering. Available()
//    reflects that; Begin() no-ops otherwise.
//  - ASYNC: the facegen rebuild is 2-10 s on a worker thread, so capture is a self-driving
//    per-frame state machine, not a blocking call — completion arrives via the DoneFn.
//  - SHARED, SERIAL: there is ONE doll; only one capture at a time (Busy()), and it MUST
//    restore the player's face before yielding.
//  - v1 FIDELITY CAVEAT: UpdateAppearance rebuilds headpart geometry from the base but not
//    hair colour / skin tint / face-sculpt morphs (open RE item), so portraits render with
//    default tint. Shipped per the brief's "ship v1 with this caveat".
//
// All engine entry points are prologue-gated; a mismatch (game patched) makes Available()
// return false and nothing is called.
namespace OSF::UI::PortraitCapture
{
	// ok = a PNG was written for the target (restore is always attempted regardless). Runs
	// on the GAME MAIN THREAD.
	using DoneFn = std::function<void(bool a_ok)>;

	// All prologue gates pass AND a live InventoryPaperDoll MenuActor exists right now (the
	// paperdoll screen is open). Cheap enough to poll before each queued capture.
	[[nodiscard]] bool Available();

	// A capture is in flight (the shared doll is mid-swap — do not start another).
	[[nodiscard]] bool Busy();

	// Start capturing a_refOrNpc (an ACHR ref formID or a TESNPC formID). a_pngNoExt is the
	// output path WITHOUT extension — the engine appends ".png". Returns false immediately
	// when unavailable, busy, or the target doesn't resolve to a TESNPC; otherwise drives
	// itself to completion and calls a_done(ok). GAME MAIN THREAD only.
	bool Begin(RE::TESFormID a_refOrNpc, const std::filesystem::path& a_pngNoExt, DoneFn a_done);
}
