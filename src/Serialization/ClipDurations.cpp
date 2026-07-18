#include "Serialization/ClipDurations.h"

#include "Registry/SceneRegistry.h"
#include "Serialization/AFImport.h"  // kAfFps
#include "Util/Ba2.h"
#include "Util/ClipPath.h"
#include "Util/Gzip.h"
#include "Util/StringUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>
#include <unordered_set>

namespace OSF::Serialization::ClipDurations
{
	namespace
	{
		using json = nlohmann::json;

		struct Entry
		{
			float         sec = 0.0f;
			std::uint64_t size = 0;   // probed source size (loose file or BA2 payload); 0 = from a decode, identity unknown
			std::int64_t  mtime = 0;  // loose-file write time (file_clock ticks); 0 = not a loose file
			bool          exact = false;  // true = from a real ozz decode (Record), not a metadata probe
		};

		std::mutex g_lock;  // guards g_map, g_loaded, g_dirty, g_scanRunning, g_rescanPending and the save
		std::unordered_map<std::string, Entry> g_map;
		bool g_loaded = false;
		bool g_dirty = false;
		bool g_scanRunning = false;
		bool g_rescanPending = false;  // a scan was requested while one ran — the worker folds in another pass

		// A probe within this of an exact decode value confirms it — keep the decode's number.
		constexpr float kExactConfirmTolerance = 0.05f;

		// Values outside (0, 1h] are treated as garbage (corrupt header / bogus accessor max) rather than persisted and shown.
		constexpr float kMaxPlausibleSec = 3600.0f;

		bool PlausibleSec(float a_sec)
		{
			return std::isfinite(a_sec) && a_sec > 0.0f && a_sec <= kMaxPlausibleSec;
		}

		// Cache key: the authored spec collapsed to its display form, the SAME identity playback uses (so "naf:X", "Data/NAF/X" and "NAF\X" all land on one entry)
		// lowercased, + the anim id. String-only (no filesystem), so it is safe inside the noexcept UI handlers.
		std::string KeyFor(std::string_view a_fileSpec, std::string_view a_animId)
		{
			return Util::ToLower(Util::ClipSpecDisplay(std::filesystem::path{ std::string{ a_fileSpec } })) +
			       '|' + std::string{ a_animId };
		}

		// <Documents>\My Games\Starfield\OSF\clip-durations.json, or empty (no persistence).
		std::filesystem::path CacheFilePath()
		{
			try {
				if (auto dir = SFSE::log::log_directory()) {
					// ...\My Games\Starfield\SFSE\Logs -> ...\My Games\Starfield\OSF
					return dir->parent_path().parent_path() / "OSF" / "clip-durations.json";
				}
			} catch (...) {}
			return {};
		}

		// ---- persistence (caller holds g_lock) -------------------------------

		void LoadLocked()
		{
			if (g_loaded) {
				return;
			}
			g_loaded = true;

			const auto file = CacheFilePath();
			if (file.empty()) {
				return;
			}
			std::ifstream in(file, std::ios::binary);
			if (!in) {
				return;  // first run
			}
			// Tolerate a corrupt/hand-edited file: bad entries are skipped, never thrown 
			try {
				const json doc = json::parse(in, nullptr, /*allow_exceptions*/ false);
				const auto clips = doc.is_object() ? doc.find("clips") : doc.end();
				if (clips == doc.end() || !clips->is_object()) {
					REX::WARN("[ClipDur] {} unreadable — starting with an empty duration cache", file.string());
					return;
				}
				for (const auto& [key, v] : clips->items()) {
					if (!v.is_object() || !v.value("sec", json{}).is_number()) {
						continue;
					}
					Entry e;
					e.sec = v["sec"].get<float>();
					e.size = v.value("size", json{}).is_number_unsigned() ? v["size"].get<std::uint64_t>() : 0;
					e.mtime = v.value("mtime", json{}).is_number_integer() ? v["mtime"].get<std::int64_t>() : 0;
					e.exact = v.value("exact", json{}).is_boolean() ? v["exact"].get<bool>() : false;
					if (PlausibleSec(e.sec)) {
						g_map.emplace(key, e);
					}
				}
			} catch (const std::exception& e) {
				g_map.clear();
				REX::WARN("[ClipDur] failed reading {} ({}) — starting with an empty duration cache", file.string(), e.what());
				return;
			}
			REX::DEBUG("[ClipDur] loaded {} cached duration(s) from {}", g_map.size(), file.string());
		}

		void SaveLocked()
		{
			if (!g_dirty) {
				return;
			}
			const auto file = CacheFilePath();
			if (file.empty()) {
				return;
			}

			json clips = json::object();
			for (const auto& [key, e] : g_map) {
				clips[key] = { { "sec", e.sec }, { "size", e.size }, { "mtime", e.mtime }, { "exact", e.exact } };
			}
			const json doc = { { "version", 1 }, { "clips", std::move(clips) } };

			std::error_code ec;
			std::filesystem::create_directories(file.parent_path(), ec);
			const auto tmp = file.parent_path() / (file.filename().string() + ".tmp");
			{
				std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
				if (!out) {
					REX::WARN("[ClipDur] cannot write {} — durations won't persist this session", tmp.string());
					return;
				}
				// replace error handler: keys are Windows file paths in the ACP narrow encoding, which need not be valid UTF-8
				out << doc.dump(1, '\t', false, json::error_handler_t::replace);
			}
			std::filesystem::rename(tmp, file, ec);
			if (ec) {
				REX::WARN("[ClipDur] cannot replace {} ({})", file.string(), ec.message());
				return;
			}
			g_dirty = false;
		}

		// ---- probes (background thread, engine-free) -------------------------

		// .af: the 64-byte header carries the clip's frame count (u16 @44 — after 8B magic, 16B rotation, 12B translation, 4B flags, 2B version, 2B boneCount; see AFImport::ParseAf).
		// Keyframe indices run 0..frameCount-1 at kAfFps, so the last frame IS the duration.
		std::optional<float> ProbeAfBytes(const std::vector<std::byte>& a_bytes)
		{
			if (a_bytes.size() < 64) {
				return std::nullopt;
			}
			std::uint16_t frameCount = 0;
			std::memcpy(&frameCount, a_bytes.data() + 44, sizeof(frameCount));
			const float lastFrame = frameCount > 1 ? static_cast<float>(frameCount - 1) : 1.0f;
			return lastFrame / AFImport::kAfFps;
		}

		// Mirrors GLTFImport::BuildLoadResult's animation selection: "" -> first, all-digits ->
		// index, anything else -> exact name match. nullptr = no such animation.
		const json* SelectAnimation(const json& a_animations, std::string_view a_animId)
		{
			if (!a_animations.is_array() || a_animations.empty()) {
				return nullptr;
			}
			if (a_animId.empty()) {
				return &a_animations[0];
			}
			if (std::all_of(a_animId.begin(), a_animId.end(),
					[](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
				std::size_t idx = 0;
				const auto* first = a_animId.data();
				if (std::from_chars(first, first + a_animId.size(), idx).ec == std::errc{} &&
					idx < a_animations.size()) {
					return &a_animations[idx];
				}
				return nullptr;
			}
			for (const auto& a : a_animations) {
				if (a.value("name", std::string{}) == a_animId) {
					return &a;
				}
			}
			return nullptr;
		}

		// GLB/glTF: a sampler's input accessor holds the keyframe times, strictly increasing per
		// spec — so the animation's duration is the LAST input element, readable straight out of
		// the binary chunk without decoding anything. Falls back to the accessor's declared `max`
		// when the value isn't reachable (external .bin buffer). NOTE: NAF-exported files carry
		// spec-violating min/max arrays (see SanitizeGltfJson), which is why the buffer read is
		// the primary source and `max` only the fallback.
		std::optional<float> ProbeGltfBytes(std::vector<std::byte> a_bytes, std::string_view a_animId)
		{
			auto bytes = Util::MaybeGunzip(std::move(a_bytes));
			if (!bytes || bytes->size() < 4) {
				return std::nullopt;
			}

			// Split into the JSON document and (for GLB) the BIN chunk.
			std::string_view jsonText;
			const std::byte* bin = nullptr;
			std::size_t binSize = 0;

			const auto readU32 = [&](std::size_t a_off) {
				std::uint32_t v = 0;
				std::memcpy(&v, bytes->data() + a_off, 4);
				return v;
			};
			constexpr std::uint32_t kGlbMagic = 0x46546C67;  // "glTF"
			constexpr std::uint32_t kJsonChunk = 0x4E4F534A;  // "JSON"
			constexpr std::uint32_t kBinChunk = 0x004E4942;   // "BIN\0"

			if (readU32(0) == kGlbMagic) {
				std::size_t off = 12;
				while (off + 8 <= bytes->size()) {
					const std::uint32_t len = readU32(off);
					const std::uint32_t type = readU32(off + 4);
					if (off + 8 + len > bytes->size()) {
						break;
					}
					if (type == kJsonChunk && jsonText.empty()) {
						jsonText = { reinterpret_cast<const char*>(bytes->data() + off + 8), len };
					} else if (type == kBinChunk && !bin) {
						bin = bytes->data() + off + 8;
						binSize = len;
					}
					off += 8 + static_cast<std::size_t>(len);
				}
			} else if (static_cast<char>((*bytes)[0]) == '{') {
				jsonText = { reinterpret_cast<const char*>(bytes->data()), bytes->size() };
			}
			if (jsonText.empty()) {
				return std::nullopt;
			}

			const json doc = json::parse(jsonText, nullptr, /*allow_exceptions*/ false);
			if (!doc.is_object()) {
				return std::nullopt;
			}
			const auto animsIt = doc.find("animations");
			if (animsIt == doc.end()) {
				return std::nullopt;
			}
			const json* anim = SelectAnimation(*animsIt, a_animId);
			if (!anim) {
				return std::nullopt;
			}
			const auto accessors = doc.find("accessors");
			const auto bufferViews = doc.find("bufferViews");

			// Last time value of one input accessor, from the BIN chunk. nullopt = unreachable.
			const auto lastInputKey = [&](const json& a_acc) -> std::optional<float> {
				if (!bin || bufferViews == doc.end() || !bufferViews->is_array()) {
					return std::nullopt;
				}
				if (a_acc.value("componentType", 0) != 5126 /*FLOAT*/ ||
					a_acc.value("type", std::string{}) != "SCALAR" || a_acc.contains("sparse")) {
					return std::nullopt;
				}
				const std::size_t count = a_acc.value("count", std::size_t{ 0 });
				const std::size_t bvIdx = a_acc.value("bufferView", std::size_t{ SIZE_MAX });
				if (count == 0 || bvIdx >= bufferViews->size()) {
					return std::nullopt;
				}
				const json& bv = (*bufferViews)[bvIdx];
				if (bv.value("buffer", std::size_t{ 0 }) != 0) {
					return std::nullopt;  // GLB binary chunk is always buffer 0
				}
				const std::size_t stride = bv.value("byteStride", std::size_t{ 4 });
				const std::size_t off = bv.value("byteOffset", std::size_t{ 0 }) +
				                        a_acc.value("byteOffset", std::size_t{ 0 }) +
				                        (count - 1) * stride;
				if (stride < 4 || off + 4 > binSize) {
					return std::nullopt;
				}
				float t = 0.0f;
				std::memcpy(&t, bin + off, 4);
				return t;
			};

			float maxTime = -1.0f;
			const auto samplers = anim->find("samplers");
			if (samplers == anim->end() || !samplers->is_array()) {
				return std::nullopt;
			}
			for (const auto& s : *samplers) {
				const std::size_t inIdx = s.value("input", std::size_t{ SIZE_MAX });
				if (accessors == doc.end() || !accessors->is_array() || inIdx >= accessors->size()) {
					continue;
				}
				const json& acc = (*accessors)[inIdx];
				std::optional<float> t = lastInputKey(acc);
				if (!t) {  // fallback: the declared max (suspect on NAF exports, see above)
					if (const auto mx = acc.find("max"); mx != acc.end() && mx->is_array() &&
						!mx->empty() && (*mx)[0].is_number()) {
						t = (*mx)[0].get<float>();
					}
				}
				if (t && std::isfinite(*t) && *t >= 0.0f) {
					maxTime = std::max(maxTime, *t);
				}
			}
			if (maxTime < 0.0f) {
				return std::nullopt;
			}
			return std::max(maxTime, 0.001f);  // matches the importer's duration floor
		}

		std::optional<std::vector<std::byte>> ReadWholeFile(const std::filesystem::path& a_file)
		{
			std::error_code ec;
			const auto sz = std::filesystem::file_size(a_file, ec);
			if (ec) {
				return std::nullopt;
			}
			std::ifstream f(a_file, std::ios::binary);
			std::vector<std::byte> buf(sz);
			if (!f || (sz > 0 && !f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz)))) {
				return std::nullopt;
			}
			return buf;
		}

		enum class ProbeOutcome : std::uint8_t
		{
			kFresh,        // cache entry matched the file identity — nothing to do
			kChanged,      // new or updated duration stored
			kInvalidated,  // source changed but yielded no duration — stale entry dropped
			kMissing,      // no readable source found
			kUnprobed      // bytes found but no duration extractable (and nothing cached to drop)
		};

		// Probe one clip ref against the cache. Runs on the scan thread; touches no engine state
		// (std IO + our own BA2 reader only — MO2's VFS virtualizes both, same as the game's view).
		ProbeOutcome ProbeOne(const std::string& a_fileSpec, const std::string& a_animId, const std::string& a_key)
		{
			const auto spec = Util::ResolveClipSpec(std::filesystem::path{ a_fileSpec });
			const bool isAf = Util::ToLower(std::filesystem::path{ a_fileSpec }.extension().string()) == ".af";

			const auto cacheFresh = [&a_key](std::uint64_t a_size, std::int64_t a_mtime) {
				std::scoped_lock l{ g_lock };
				const auto it = g_map.find(a_key);
				return it != g_map.end() && it->second.size == a_size && it->second.mtime == a_mtime;
			};

			// Locate the source: first loose file across the candidates (mirrors the engine's
			// loose-over-archive precedence), then — for .af only — the game BA2s. GLBs packed
			// into mod BA2s stay unknown here and get backfilled by Record on first playback
			// (Ba2::ReadGameFile re-scans every archive per call — too slow for a bulk pass).
			std::vector<std::byte> bytes;
			std::uint64_t size = 0;
			std::int64_t mtime = 0;
			bool found = false;

			for (const auto& cand : spec.candidates) {
				std::error_code ec;
				if (!std::filesystem::is_regular_file(cand.filePath, ec)) {
					continue;
				}
				size = std::filesystem::file_size(cand.filePath, ec);
				mtime = ec ? 0 : std::filesystem::last_write_time(cand.filePath, ec).time_since_epoch().count();
				if (cacheFresh(size, mtime)) {
					return ProbeOutcome::kFresh;
				}
				if (auto b = ReadWholeFile(cand.filePath)) {
					bytes = std::move(*b);
					found = true;
				}
				break;
			}

			if (!found && isAf) {
				for (const auto& cand : spec.candidates) {
					if (!cand.resource) {
						continue;
					}
					if (auto b = Util::Ba2::ReadGameFile(cand.resourcePath, "Starfield - Animations.ba2")) {
						bytes.resize(b->size());
						if (!b->empty()) {
							std::memcpy(bytes.data(), b->data(), b->size());
						}
						size = bytes.size();
						mtime = 0;
						found = true;
						break;
					}
				}
				if (found && cacheFresh(size, 0)) {
					return ProbeOutcome::kFresh;
				}
			}

			if (!found) {
				return ProbeOutcome::kMissing;
			}

			const auto sec = isAf ? ProbeAfBytes(bytes) : ProbeGltfBytes(std::move(bytes), a_animId);
			if (!sec || !PlausibleSec(*sec)) {
				// The source changed AND we can't read a duration out of it — drop any stale value.
				std::scoped_lock l{ g_lock };
				if (g_map.erase(a_key) > 0) {
					g_dirty = true;
					return ProbeOutcome::kInvalidated;
				}
				return ProbeOutcome::kUnprobed;
			}

			std::scoped_lock l{ g_lock };
			auto& e = g_map[a_key];
			const bool confirmsExact = e.exact && std::abs(e.sec - *sec) <= kExactConfirmTolerance;
			const bool valueChanged = !confirmsExact && e.sec != *sec;
			if (!confirmsExact) {
				e.sec = *sec;
				e.exact = false;
			}
			e.size = size;
			e.mtime = mtime;
			g_dirty = true;  // identity refresh persists even when the value held
			return valueChanged ? ProbeOutcome::kChanged : ProbeOutcome::kFresh;
		}

		// One full pass: gather refs from the live registry (its own read lock, NEVER under
		// g_lock — BuildCatalog nests g_lock inside the registry lock, so nesting the other way
		// around here could deadlock through a queued LoadAll writer), probe them, prune, save.
		// Returns the number of catalog-visible changes.
		std::size_t ScanPass()
		{
			const auto t0 = std::chrono::steady_clock::now();

			std::vector<std::pair<std::string, std::string>> refs;
			Registry::SceneRegistry::GetSingleton().ForEachDef([&refs](const Registry::SceneDef& d) {
				if (!d.clipsAvailable) {
					return;  // clips not installed — probing would just fail-log per file
				}
				for (const auto& n : d.nodes) {
					for (const auto& st : n.stages) {
						for (const auto& c : st.clips) {
							// A pack-authored `sec` needs no probe — keeps the thousands of generated
							// vanilla-pack refs from re-walking the game archive every scan.
							if (!c.file.empty() && c.sec <= 0.0f) {
								refs.emplace_back(c.file, c.animId);
							}
						}
					}
				}
			});

			{
				std::scoped_lock l{ g_lock };
				LoadLocked();
			}

			std::unordered_set<std::string> seen;
			std::size_t fresh = 0, changed = 0, missing = 0, unprobed = 0;
			for (const auto& [file, animId] : refs) {
				auto key = KeyFor(file, animId);
				if (!seen.insert(key).second) {
					continue;
				}
				switch (ProbeOne(file, animId, key)) {
				case ProbeOutcome::kFresh:       fresh++; break;
				case ProbeOutcome::kChanged:     changed++; break;
				case ProbeOutcome::kInvalidated: changed++; unprobed++; break;
				case ProbeOutcome::kMissing:     missing++; break;
				case ProbeOutcome::kUnprobed:    unprobed++; break;
				}
			}

			std::size_t pruned = 0;
			{
				std::scoped_lock l{ g_lock };
				// Prune probe entries no clip reference uses any more (uninstalled/renamed packs)
				// so the cache tracks the install instead of growing forever. Exact entries stay:
				// they may belong to ad-hoc plays (OSF.Play with a bare path) outside any scene.
				for (auto it = g_map.begin(); it != g_map.end();) {
					if (!it->second.exact && !seen.contains(it->first)) {
						it = g_map.erase(it);
						g_dirty = true;
						pruned++;
					} else {
						++it;
					}
				}
				SaveLocked();
			}

			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - t0).count();
			REX::INFO("[ClipDur] duration scan: {} unique clip(s) — {} fresh, {} updated, {} missing, {} unprobeable, {} pruned ({} ms)",
				seen.size(), fresh, changed, missing, unprobed, pruned, ms);
			return changed;
		}
	}

	std::optional<float> Lookup(std::string_view a_fileSpec, std::string_view a_animId)
	{
		const auto key = KeyFor(a_fileSpec, a_animId);
		std::scoped_lock l{ g_lock };
		LoadLocked();
		const auto it = g_map.find(key);
		if (it == g_map.end()) {
			return std::nullopt;
		}
		return it->second.sec;
	}

	void Record(std::string_view a_fileSpec, std::string_view a_animId, float a_seconds)
	{
		if (!PlausibleSec(a_seconds)) {
			return;
		}
		const auto key = KeyFor(a_fileSpec, a_animId);
		std::scoped_lock l{ g_lock };
		LoadLocked();
		const auto it = g_map.find(key);
		if (it != g_map.end()) {
			auto& e = it->second;
			const bool close = std::abs(e.sec - a_seconds) <= kExactConfirmTolerance;
			if (e.exact && close) {
				return;  // already have this number
			}
			e.sec = a_seconds;
			e.exact = true;
			g_dirty = true;
			if (close) {
				return;  // probe confirmed, value unchanged — persist lazily (next scan save),
				         // not one whole-file rewrite per clip while a scene preloads its stages
			}
		} else {
			auto& e = g_map[key];
			e.sec = a_seconds;
			e.exact = true;
			g_dirty = true;
		}
		SaveLocked();  // genuinely new information — worth the write
	}

	void ScanSceneClipsAsync(std::function<void()> a_onChanged)
	{
		{
			std::scoped_lock l{ g_lock };
			if (g_scanRunning) {
				// Fold into the running scan: its worker re-gathers from the (possibly reloaded)
				// registry and runs another pass before finishing, using its own callback.
				g_rescanPending = true;
				REX::DEBUG("[ClipDur] scan already running — queued a follow-up pass");
				return;
			}
			g_scanRunning = true;
		}

		std::thread([onChanged = std::move(a_onChanged)]() {
			std::size_t changed = 0;
			for (;;) {
				// A detached thread must not leak exceptions (std::terminate). Failures here only cost estimates, never the game.
				try {
					changed += ScanPass();
				} catch (const std::exception& e) {
					REX::ERROR("[ClipDur] duration scan failed: {}", e.what());
				} catch (...) {
					REX::ERROR("[ClipDur] duration scan failed (non-std exception)");
				}
				std::scoped_lock l{ g_lock };
				if (!g_rescanPending) {
					g_scanRunning = false;
					break;
				}
				g_rescanPending = false;
			}
			if (changed > 0 && onChanged) {
				SFSE::GetTaskInterface()->AddTask([cb = std::move(onChanged)]() { cb(); });
			}
		}).detach();
	}
}
