#include "UI/PortraitService.h"

#include "Matchmaking/Matchmaker.h"  // ActorGenderTag (appearance-key ingredient)

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

		// ---- capture backend ----------------------------------------------------

		// The RE-pending piece: render ONE loaded actor's headshot to a_pngOut. The
		// mechanism (BSMenu3D paperdoll retarget / ImageCapture 39202 + camera) is being
		// proven in OSF RE (Requests/2026-07-02-actor-portrait-capture); until an anchor
		// set lands here this always fails and the pipeline serves disk cache only.
		bool CaptureHeadshot(RE::Actor* a_actor, const std::filesystem::path& a_pngOut)
		{
			(void)a_actor;
			(void)a_pngOut;
			static bool logged = false;
			if (!logged) {
				logged = true;
				REX::DEBUG("[UI] portrait capture backend not available yet — serving disk-cached portraits only");
			}
			return false;
		}

		// ---- queue pump (one capture per task ~= per frame) ---------------------

		void ArmPump();

		void ServiceOne()
		{
			g_pumpArmed = false;
			if (g_queue.empty()) {
				return;
			}
			const std::string key = std::move(g_queue.front());
			g_queue.pop_front();
			auto waiting = g_waiting.extract(key);

			// Re-resolve a still-live actor from the refs that asked for this key —
			// pointers are never held across frames (same discipline as ResolveToken).
			RE::Actor* actor = nullptr;
			if (!waiting.empty()) {
				for (const RE::TESFormID id : waiting.mapped()) {
					// Actors are ACHR refs; the type check guards form-id reuse since the request.
					auto* form = RE::TESForm::LookupByID(id);
					if (form && form->Is(RE::FormType::kACHR) && !form->IsDeleted()) {
						actor = static_cast<RE::Actor*>(form);
						break;
					}
				}
			}

			const auto png = PngPathFor(key);
			bool       ok = false;
			if (actor && !png.empty()) {
				std::error_code ec;
				std::filesystem::create_directories(png.parent_path(), ec);
				ok = !ec && CaptureHeadshot(actor, png);
			}
			if (ok) {
				if (std::string uri = LoadDataUri(png); !uri.empty()) {
					g_memory[key] = uri;
					if (g_sink && !waiting.empty()) {
						for (const RE::TESFormID id : waiting.mapped()) {
							g_sink(id, uri);
						}
					}
					REX::DEBUG("[UI] portrait {} captured ({} waiting row(s))", key,
						waiting.empty() ? 0 : waiting.mapped().size());
				} else {
					ok = false;
				}
			}
			if (!ok) {
				g_failed.insert(key);  // don't retry this session; ResetSession clears
			}
			if (!g_queue.empty()) {
				ArmPump();
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
