#include "Util/NativeMainThreadQueue.h"
#include "Serialization/ClipDurationCatalog.h"
#include "Serialization/ClipDurationState.h"

#include "Registry/ContentRegistry.h"
#include "Serialization/AFImport.h"
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

namespace OSF::Serialization::ClipDurationCatalog
{
	namespace
	{
		using json = nlohmann::json;
		using namespace ClipDurations::Detail;

		struct ProcessState
		{
			std::mutex lock;
			bool loaded = false;
			std::uint64_t publishedRevision = 0; // scan worker only
			bool scanRunning = false;
			bool rescanPending = false;
		};

		// Leaked: the detached scanner may still be running during CRT teardown.
		ProcessState& g_state = *new ProcessState();

		// Empty = no persistence.
		std::filesystem::path CacheFilePath()
		{
			try {
				if (auto dir = SFSE::log::log_directory()) {
					return dir->parent_path() / "OSF" / "clip-durations.json";
				}
			} catch (...) {}
			return {};
		}

		// Single scan worker. Disk data never overwrites durations recorded in this process.
		std::size_t LoadCache()
		{
			if (g_state.loaded) {
				return 0;
			}
			g_state.loaded = true;

			const auto file = CacheFilePath();
			if (file.empty()) {
				return 0;
			}
			std::ifstream in(file, std::ios::binary);
			if (!in) {
				return 0;
			}
			std::unordered_map<std::string, Entry> entries;
			try {
				const json doc = json::parse(in, nullptr, /*allow_exceptions*/ false);
				const auto clips = doc.is_object() ? doc.find("clips") : doc.end();
				if (clips == doc.end() || !clips->is_object()) {
					REX::WARN("[Registry] clip-duration cache {} unreadable — starting empty", file.string());
					return 0;
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
						entries.emplace(key, e);
					}
				}
			} catch (const std::exception& e) {
				REX::WARN("[Registry] clip-duration cache read failed for {} ({}) — starting empty", file.string(), e.what());
				return 0;
			}
			std::size_t added = 0;
			{
				auto& state = GetState();
				std::scoped_lock lock{ state.lock };
				for (const auto& [key, entry] : entries) added += state.entries.emplace(key, entry).second;
			}
			REX::DEBUG("[Registry] loaded {} cached clip duration(s) from {}", added, file.string());
			return added;
		}

		void SaveCache()
		{
			auto& state = GetState();
			std::unordered_map<std::string, Entry> entries;
			std::uint64_t revision;
			{
				std::scoped_lock lock{ state.lock };
				if (state.revision == state.savedRevision) return;
				entries = state.entries;
				revision = state.revision;
			}
			const auto file = CacheFilePath();
			if (file.empty()) {
				return;
			}

			json clips = json::object();
			for (const auto& [key, e] : entries) {
				clips[key] = { { "sec", e.sec }, { "size", e.size }, { "mtime", e.mtime }, { "exact", e.exact } };
			}
			const json doc = { { "version", 1 }, { "clips", std::move(clips) } };

			std::error_code ec;
			std::filesystem::create_directories(file.parent_path(), ec);
			const auto tmp = file.parent_path() / (file.filename().string() + ".tmp");
			{
				std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
				if (!out) {
					REX::WARN("[Registry] cannot write clip-duration cache {} — values won't persist this session", tmp.string());
					return;
				}
				// Keys are ACP-encoded paths, not necessarily valid UTF-8.
				out << doc.dump(1, '\t', false, json::error_handler_t::replace);
				out.close();
				if (!out) {
					REX::WARN("[Registry] cannot finish clip-duration cache {} — keeping pending values", tmp.string());
					return;
				}
			}
			std::filesystem::rename(tmp, file, ec);
			if (ec) {
				REX::WARN("[Registry] cannot replace clip-duration cache {} ({})", file.string(), ec.message());
				return;
			}
			std::scoped_lock lock{ state.lock };
			state.savedRevision = revision; // New records remain dirty if playback ran during the write.
		}

		// .af header: u16 frameCount @44 (see AFImport::ParseAf); duration = (frameCount - 1) / kAfFps.
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

		// Must mirror GLTFImport::BuildLoadResult's selection: "" = first, digits = index, else name.
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

		// Duration = last element of each sampler's input accessor (times are strictly increasing).
		// Accessor `max` is only a fallback: NAF exports carry bogus min/max (see SanitizeGltfJson).
		std::optional<float> ProbeGltfBytes(std::vector<std::byte> a_bytes, std::string_view a_animId)
		{
			auto bytes = Util::MaybeGunzip(std::move(a_bytes));
			if (!bytes || bytes->size() < 4) {
				return std::nullopt;
			}

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
				const std::size_t bvOff = bv.value("byteOffset", std::size_t{ 0 });
				const std::size_t accOff = a_acc.value("byteOffset", std::size_t{ 0 });
				// Bounds checked stepwise: all terms are file-declared and a wrapped sum would read out of bounds.
				if (stride < 4 || bvOff > binSize || accOff > binSize - bvOff) {
					return std::nullopt;
				}
				const std::size_t base = bvOff + accOff;
				const std::size_t avail = binSize - base;
				if (avail < 4 || count - 1 > (avail - 4) / stride) {
					return std::nullopt;
				}
				const std::size_t off = base + (count - 1) * stride;
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
				if (!t) {
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
			if (ec || sz > Util::kMaxClipBytes) {
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
			kFresh,
			kChanged,
			kInvalidated,  // source changed but yielded no duration; stale entry dropped
			kMissing,
			kUnprobed
		};

		// Scan thread: std IO + own BA2 reader only, no engine state.
		ProbeOutcome ProbeOne(const std::string& a_fileSpec, const std::string& a_animId, const std::string& a_key)
		{
			auto& state = GetState();
			std::uint64_t decodedRevision = 0;
			{
				std::scoped_lock lock{ state.lock };
				if (const auto it = state.entries.find(a_key); it != state.entries.end()) decodedRevision = it->second.decodedRevision;
			}
			const auto spec = Util::ResolveClipSpec(std::filesystem::path{ a_fileSpec });
			const bool isAf = Util::ToLower(std::filesystem::path{ a_fileSpec }.extension().string()) == ".af";

			const auto cacheFresh = [&](std::uint64_t a_size, std::int64_t a_mtime) {
				std::scoped_lock lock{ state.lock };
				const auto it = state.entries.find(a_key);
				return it != state.entries.end() && it->second.size == a_size && it->second.mtime == a_mtime;
			};

			// Loose files first, then game BA2s for .af only. GLBs in mod BA2s are backfilled by Record
			// (ReadGameFile rescans every archive per call, too slow for a bulk pass).
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
			std::scoped_lock lock{ state.lock };
			const auto current = state.entries.find(a_key);
			if (current != state.entries.end() && current->second.decodedRevision != decodedRevision) {
				return ProbeOutcome::kFresh; // Playback recorded a newer value while the probe was reading.
			}
			if (!sec || !PlausibleSec(*sec)) {
				if (state.entries.erase(a_key) > 0) {
					++state.revision;
					return ProbeOutcome::kInvalidated;
				}
				return ProbeOutcome::kUnprobed;
			}

			auto& e = state.entries[a_key];
			const bool confirmsExact = e.exact && std::abs(e.sec - *sec) <= kExactConfirmTolerance;
			const bool valueChanged = !confirmsExact && e.sec != *sec;
			if (!confirmsExact) {
				e.sec = *sec;
				e.exact = false;
			}
			e.size = size;
			e.mtime = mtime;
			++state.revision;
			return valueChanged ? ProbeOutcome::kChanged : ProbeOutcome::kFresh;
		}

		// Returns catalog-visible changes, including durations first loaded from disk.
		std::size_t ScanPass()
		{
			const auto t0 = std::chrono::steady_clock::now();

			std::vector<std::pair<std::string, std::string>> refs;
			Registry::ContentRegistry::GetSingleton().ForEachDef([&refs](const Registry::SceneDef& d) {
				if (!d.clipsAvailable) {
					return;
				}
				for (const auto& n : d.nodes) {
					for (const auto& st : n.stages) {
						for (const auto& c : st.clips) {
							// Pack-authored `sec` skips the probe (vanilla packs would rewalk the game BA2 every scan).
							if (!c.file.empty() && c.sec <= 0.0f) {
								refs.emplace_back(c.file, c.animId);
							}
						}
					}
				}
			});

			std::unordered_set<std::string> seen;
			std::size_t fresh = 0, changed = LoadCache(), missing = 0, unprobed = 0;
			for (const auto& [file, animId] : refs) {
				auto key = KeyFor(file, animId);
				if (!seen.insert(key).second) {
					continue;
				}
				// Per-file guard so one malformed clip doesn't abort the pass.
				ProbeOutcome outcome = ProbeOutcome::kUnprobed;
				try {
					outcome = ProbeOne(file, animId, key);
				} catch (const std::exception& e) {
					REX::WARN("[Registry] clip-duration probe failed for '{}': {}", file, e.what());
				} catch (...) {
					REX::WARN("[Registry] clip-duration probe failed for '{}' (non-std exception)", file);
				}
				switch (outcome) {
				case ProbeOutcome::kFresh:       fresh++; break;
				case ProbeOutcome::kChanged:     changed++; break;
				case ProbeOutcome::kInvalidated: changed++; unprobed++; break;
				case ProbeOutcome::kMissing:     missing++; break;
				case ProbeOutcome::kUnprobed:    unprobed++; break;
				}
			}

			std::size_t pruned = 0;
			{
				auto& state = GetState();
				std::scoped_lock lock{ state.lock };
				// Exact entries survive pruning: they may come from ad-hoc OSF.Play paths outside any scene.
				for (auto it = state.entries.begin(); it != state.entries.end();) {
					if (!it->second.exact && !seen.contains(it->first)) {
						it = state.entries.erase(it);
						++state.revision;
						pruned++;
					} else {
						++it;
					}
				}
			}
			SaveCache();
			bool memoryChanged;
			{
				auto& state = GetState();
				std::scoped_lock lock{ state.lock };
				memoryChanged = state.revision != g_state.publishedRevision;
				g_state.publishedRevision = state.revision;
			}

			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - t0).count();
			REX::INFO("[Registry] duration scan: {} unique clip(s) — {} fresh, {} updated, {} missing, {} unprobeable, {} pruned ({} ms)",
				seen.size(), fresh, changed, missing, unprobed, pruned, ms);
			return changed + pruned + memoryChanged;
		}
	}

	void ScanSceneClipsAsync(std::function<void()> a_onChanged)
	{
		{
			std::scoped_lock lock{ g_state.lock };
			if (g_state.scanRunning) {
				// The running worker does another pass with its own callback.
				g_state.rescanPending = true;
				REX::DEBUG("[Registry] clip-duration scan already running — queued a follow-up pass");
				return;
			}
			g_state.scanRunning = true;
		}

		try {
			std::thread([onChanged = std::move(a_onChanged)]() {
				std::size_t changed = 0;
				for (;;) {
					try {
						changed += ScanPass();
					} catch (const std::exception& e) {
						REX::ERROR("[Registry] clip-duration scan failed: {}", e.what());
					} catch (...) {
						REX::ERROR("[Registry] clip-duration scan failed (non-std exception)");
					}
					std::scoped_lock lock{ g_state.lock };
					if (!g_state.rescanPending) {
						g_state.scanRunning = false;
						break;
					}
					g_state.rescanPending = false;
				}
				if (changed > 0 && onChanged) {
					Util::NativeMainThreadQueue::Post([cb = std::move(onChanged)]() { cb(); });
				}
			}).detach();
		} catch (...) {
			std::scoped_lock lock{ g_state.lock };
			g_state.scanRunning = false;
			g_state.rescanPending = false;
			REX::ERROR("[Registry] could not start clip-duration scan");
		}
	}
}
