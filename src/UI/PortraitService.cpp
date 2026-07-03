#include "UI/PortraitService.h"

#include "Matchmaking/Matchmaker.h"  // ActorGenderTag (appearance-key ingredient)
#include "UI/PortraitCapture.h"      // the async, menu-bound capture backend

#include <algorithm>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OSF::UI::Portraits
{
	namespace
	{
		// All state below is GAME MAIN THREAD only (the GetOrRequest contract + SFSE tasks),
		// so no locking — same model as UIBridge's token table.

		ReadySink g_sink;

		// key -> data URI, memoized for the session (a portrait is ~10-40 KB encoded;
		// bounded by distinct NPCs scanned, so no eviction).
		std::unordered_map<std::string, std::string> g_memory;

		// Keys whose capture failed this session — don't re-queue them every scan.
		std::unordered_set<std::string> g_failed;

		// Capture queue: FIFO of keys + the refs waiting on each key (two refs sharing a
		// template face share a key, and each waiting row wants its own sink call).
		std::deque<std::string>                                        g_queue;
		std::unordered_map<std::string, std::vector<RE::TESFormID>>    g_waiting;
		bool                                                           g_pumpArmed = false;

		// ---- identity ---------------------------------------------------------

		// Cache key = base-NPC form id + an appearance hash. The base id (not the ref's)
		// makes template-face NPCs (guards, crowds) share one portrait. The hash is what
		// must invalidate the cached image when the face could differ: v1 folds in race +
		// sex only — headparts/morph state can join once the capture backend exists and
		// the cheapest correct source for them is known.
		std::string KeyFor(RE::Actor* a_actor)
		{
			const auto          base = a_actor->GetBaseObject();  // NiPointer<TESBoundObject>
			const RE::TESFormID baseID = base ? base->GetFormID() : a_actor->GetFormID();
			std::uint32_t       hash = 2166136261u;  // FNV-1a over the appearance fields
			const auto          mix = [&hash](std::uint32_t a_v) {
                for (int i = 0; i < 4; ++i) {
                    hash ^= (a_v >> (i * 8)) & 0xFFu;
                    hash *= 16777619u;
                }
			};
			mix(a_actor->race ? a_actor->race->GetFormID() : 0);
			const std::string gender = Matchmaking::ActorGenderTag(a_actor);
			for (const char c : gender) {
				hash ^= static_cast<unsigned char>(c);
				hash *= 16777619u;
			}
			return std::format("{:08x}-{:08x}", baseID, hash);
		}

		// <Documents>\My Games\Starfield\OSF\Portraits, or empty (no disk cache).
		std::filesystem::path CacheDir()
		{
			try {
				if (auto dir = SFSE::log::log_directory()) {
					// ...\My Games\Starfield\SFSE\Logs -> ...\My Games\Starfield\OSF
					return dir->parent_path().parent_path() / "OSF" / "Portraits";
				}
			} catch (...) {}
			return {};
		}

		std::filesystem::path PngPathFor(const std::string& a_key)
		{
			const auto dir = CacheDir();
			return dir.empty() ? dir : dir / (a_key + ".png");
		}

		// ---- encoding ---------------------------------------------------------

		std::string Base64(const std::vector<unsigned char>& a_bytes)
		{
			static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::string           out;
			out.reserve((a_bytes.size() + 2) / 3 * 4);
			std::size_t i = 0;
			for (; i + 2 < a_bytes.size(); i += 3) {
				const std::uint32_t n = (a_bytes[i] << 16) | (a_bytes[i + 1] << 8) | a_bytes[i + 2];
				out += kTable[(n >> 18) & 63];
				out += kTable[(n >> 12) & 63];
				out += kTable[(n >> 6) & 63];
				out += kTable[n & 63];
			}
			if (const std::size_t rest = a_bytes.size() - i; rest == 1) {
				const std::uint32_t n = a_bytes[i] << 16;
				out += kTable[(n >> 18) & 63];
				out += kTable[(n >> 12) & 63];
				out += "==";
			} else if (rest == 2) {
				const std::uint32_t n = (a_bytes[i] << 16) | (a_bytes[i + 1] << 8);
				out += kTable[(n >> 18) & 63];
				out += kTable[(n >> 12) & 63];
				out += kTable[(n >> 6) & 63];
				out += '=';
			}
			return out;
		}

		// PNG file -> "data:image/png;base64,…", or empty (missing/unreadable/oversized).
		std::string LoadDataUri(const std::filesystem::path& a_png)
		{
			// A portrait past this is not a thumbnail — refuse rather than shipping
			// megabytes of JSON through the bridge per row.
			constexpr std::uintmax_t kMaxPngBytes = 512 * 1024;
			std::error_code          ec;
			const auto               size = std::filesystem::file_size(a_png, ec);
			if (ec || size == 0 || size > kMaxPngBytes) {
				return {};
			}
			std::ifstream in(a_png, std::ios::binary);
			if (!in) {
				return {};
			}
			std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
			in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			if (!in) {
				return {};
			}
			return "data:image/png;base64," + Base64(bytes);
		}

		// ---- capture backend (async, menu-bound) --------------------------------
		// The real backend renders on the LIVE inventory paperdoll (PortraitCapture), so a
		// capture only runs while that screen is open, takes seconds, and mutates one shared
		// doll. Two consequences for the queue: captures are ARMED explicitly (default off =
		// cache-only, the safe shipped behaviour — auto-capturing would silently hijack the
		// player's doll), and serviced one at a time when the paperdoll is actually open.

		void ArmPump();  // fwd (BeginCaptureFor's completion re-arms the pump)

		bool g_captureArmed = false;  // SetCaptureEnabled; gates the queue's real captures
		int  g_idleTicks = 0;         // armed but nothing capturable — auto-disarm after a while

		// Armed-but-no-paperdoll: stop spinning a per-frame task after ~this long (the trigger
		// re-arms when it next has a capture context).
		constexpr int kMaxIdleTicks = 900;  // ~15 s

		// Resolve a still-live ACHR from a set of ref form ids (pointers are never held across
		// frames). 0 = none live.
		RE::TESFormID FirstLiveRef(const std::vector<RE::TESFormID>& a_ids)
		{
			for (const RE::TESFormID id : a_ids) {
				auto* form = RE::TESForm::LookupByID(id);
				if (form && form->Is(RE::FormType::kACHR) && !form->IsDeleted()) {
					return id;
				}
			}
			return 0;
		}

		// Kick off one capture for a_key on behalf of a_notify refs. On completion (main
		// thread) cache the PNG data URI and fan out to every waiting row. Returns false when
		// the capture couldn't even start (caller keeps the key queued / negative-caches).
		bool BeginCaptureFor(const std::string& a_key, RE::TESFormID a_ref,
			std::vector<RE::TESFormID> a_notify)
		{
			const auto stem = PngPathFor(a_key);  // "<dir>/<key>.png"; PortraitCapture appends ".png"
			if (stem.empty()) {
				return false;
			}
			std::error_code ec;
			std::filesystem::create_directories(stem.parent_path(), ec);
			auto pngNoExt = stem;
			pngNoExt.replace_extension();

			return PortraitCapture::Begin(a_ref, pngNoExt,
				[key = a_key, notify = std::move(a_notify)](bool a_ok) {
					if (a_ok) {
						if (std::string uri = LoadDataUri(PngPathFor(key)); !uri.empty()) {
							g_memory[key] = uri;
							if (g_sink) {
								for (const RE::TESFormID id : notify) {
									g_sink(id, uri);
								}
							}
							REX::DEBUG("[UI] portrait {} captured ({} waiting row(s))", key, notify.size());
						} else {
							a_ok = false;
						}
					}
					if (!a_ok) {
						g_failed.insert(key);  // don't retry this session
					}
					ArmPump();  // service the next queued key
				});
		}

		// ---- queue pump ---------------------------------------------------------
		// One SFSE task ~= one frame. While armed it advances at most one capture start per
		// tick (the capture itself is async and re-arms us when it finishes).

		void ServiceOne()
		{
			g_pumpArmed = false;
			if (g_queue.empty() || !g_captureArmed) {
				g_idleTicks = 0;
				return;  // cache-only until armed; nothing to do
			}
			if (PortraitCapture::Busy()) {
				ArmPump();  // a capture is mid-flight; wait for its done-callback
				return;
			}
			if (!PortraitCapture::Available()) {
				// Armed, but the paperdoll isn't open (or the engine gate failed). Watch for a
				// while, then stand down so we don't burn a task every frame indefinitely.
				if (++g_idleTicks >= kMaxIdleTicks) {
					g_captureArmed = false;
					g_idleTicks = 0;
					REX::DEBUG("[UI] portrait capture idle (paperdoll not open) — disarmed; re-arm to resume");
					return;
				}
				ArmPump();
				return;
			}
			g_idleTicks = 0;

			// Pop the next key that still has a live ref; drop dead ones.
			while (!g_queue.empty()) {
				const std::string key = std::move(g_queue.front());
				g_queue.pop_front();
				std::vector<RE::TESFormID> refs;
				if (auto node = g_waiting.extract(key)) {
					refs = std::move(node.mapped());
				}
				const RE::TESFormID ref = FirstLiveRef(refs);
				if (ref == 0) {
					continue;  // everyone who wanted it is gone; skip
				}
				if (BeginCaptureFor(key, ref, std::move(refs))) {
					return;  // in flight; its done-callback re-arms the pump
				}
				g_failed.insert(key);  // couldn't start (bad target) — don't spin on it
			}
		}

		void ArmPump()
		{
			if (g_pumpArmed) {
				return;
			}
			g_pumpArmed = true;
			SFSE::GetTaskInterface()->AddTask([]() { ServiceOne(); });
		}
	}

	void SetCaptureEnabled(bool a_on)
	{
		g_captureArmed = a_on;
		g_idleTicks = 0;
		REX::INFO("[UI] portrait capture {}", a_on ? "armed" : "disarmed");
		if (a_on) {
			ArmPump();
		}
	}

	bool CaptureNow(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		// This is reachable from the Papyrus VM thread (the CapturePortrait native), but every
		// engine touch in PortraitCapture must run on the game main thread — so marshal there.
		// Capture only the form id (re-resolved on the task); a raw actor pointer must not cross.
		const RE::TESFormID ref = a_actor->GetFormID();
		SFSE::GetTaskInterface()->AddTask([ref]() {
			auto* form = RE::TESForm::LookupByID(ref);
			if (!form || !form->Is(RE::FormType::kACHR) || form->IsDeleted()) {
				return;
			}
			auto* actor = static_cast<RE::Actor*>(form);
			BeginCaptureFor(KeyFor(actor), ref, { ref });
		});
		return true;  // request accepted; the capture runs (and reports to the cache) next frame
	}

	void SetReadySink(ReadySink a_sink)
	{
		g_sink = std::move(a_sink);
	}

	std::string GetOrRequest(RE::Actor* a_actor, RE::TESFormID a_refFormID)
	{
		if (!a_actor) {
			return {};
		}
		const std::string key = KeyFor(a_actor);

		if (const auto it = g_memory.find(key); it != g_memory.end()) {
			return it->second;
		}
		// Disk tier: a PNG under the key (captured a past session, or hand-dropped).
		if (const auto png = PngPathFor(key); !png.empty()) {
			if (std::string uri = LoadDataUri(png); !uri.empty()) {
				g_memory[key] = uri;
				return uri;
			}
		}
		if (g_failed.contains(key)) {
			return {};
		}
		// Queue a capture; dedup on key, but record every ref waiting on it.
		auto& waiting = g_waiting[key];
		if (waiting.empty()) {
			g_queue.push_back(key);
		}
		if (std::find(waiting.begin(), waiting.end(), a_refFormID) == waiting.end()) {
			waiting.push_back(a_refFormID);
		}
		ArmPump();
		return {};
	}

	void ResetSession()
	{
		g_memory.clear();
		g_failed.clear();
		g_queue.clear();
		g_waiting.clear();
	}
}
