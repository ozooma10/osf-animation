#pragma once

#include <functional>
#include <string>

// Actor portrait pipeline for the scene browser's scan list: a two-tier cache
// (in-memory data URIs over <Documents>\My Games\Starfield\OSF\Portraits\<key>.png)
// fed by a dedup'd capture queue serviced ~one actor per frame on the game main
// thread. The capture backend (single-actor offscreen headshot) is the RE-pending
// piece — BSMenu3D paperdoll + ImageCapture, see OSF RE Investigations/Requests
// 2026-07-02-actor-portrait-capture — and is stubbed until that mechanism lands;
// meanwhile the pipeline serves disk-cached portraits (a PNG dropped into the
// Portraits folder under the right key shows up in-game).
namespace OSF::UI::Portraits
{
	// Fired on the GAME MAIN THREAD when a queued capture lands: the ref the portrait
	// was requested for + a ready-to-render "data:image/png;base64,…" URI.
	using ReadySink = std::function<void(RE::TESFormID a_refFormID, const std::string& a_dataUri)>;

	// Install the completion sink (UIBridge). Replaces any prior sink. Main thread.
	void SetReadySink(ReadySink a_sink);

	// Cache-or-queue, the one entry point (GAME MAIN THREAD). Returns the portrait as a
	// data URI when it is already available (memory or disk); otherwise returns an empty
	// string and — unless the key is negative-cached from an earlier failed capture —
	// queues a capture that will announce itself through the ReadySink.
	std::string GetOrRequest(RE::Actor* a_actor, RE::TESFormID a_refFormID);

	// Arm/disarm the queue's REAL captures (default OFF = cache-only). While armed, queued
	// misses are captured on the live inventory paperdoll one at a time whenever it is open;
	// disarmed, the pipeline only serves already-cached portraits. Off by default because a
	// capture visibly hijacks the player's paperdoll — it must be an explicit, bounded action.
	void SetCaptureEnabled(bool a_on);

	// Capture ONE actor's portrait right now (the acceptance-test / direct path), bypassing
	// the queue. Requires the inventory paperdoll open (PortraitCapture::Available); returns
	// false if the capture couldn't start. Result lands in the cache + sink like any other.
	bool CaptureNow(RE::Actor* a_actor);

	// Drop the session caches (memory + negative cache + queue); the disk cache stays.
	// Lets a reload pick up externally added PNGs without relaunching.
	void ResetSession();
}
