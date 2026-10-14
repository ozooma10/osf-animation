#include "Serialization/ClipDurationCache.h"
#include "API/EngineClient.h"
#include <cmath>
#include <fstream>
#include <thread>
#include <unordered_set>

namespace OSF::Browser::ClipDurationCache
{
	namespace
	{
		using json = nlohmann::json;
		bool Plausible(float sec) { return std::isfinite(sec) && sec > 0 && sec <= 3600; }
		struct Entry
		{
			float         sec{};
			std::uint64_t size{};
			std::int64_t  mtime{};
			bool          exact{};
		};
		struct CacheState
		{
			std::mutex                             lock;
			std::unordered_map<std::string, Entry> entries;
			std::uint64_t                          revision{}, savedRevision{};
		};
		CacheState& State()
		{
			static auto* state = new CacheState();
			return *state;
		}
		struct WorkerState
		{
			std::mutex                                lock;
			bool                                      loaded{}, running{};
			std::optional<std::vector<Catalog::Clip>> pending;
			std::map<std::string, std::uint64_t>      decodedRevisions;
		};
		// The provider and addon stay loaded for process lifetime, including detached worker teardown.
		WorkerState& g_state = *new WorkerState();
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
					REX::WARN("[UI] clip-duration cache {} unreadable — starting empty", file.string());
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
					if (Plausible(e.sec)) {
						entries.emplace(key, e);
					}
				}
			} catch (const std::exception& e) {
				REX::WARN("[UI] clip-duration cache read failed for {} ({}) — starting empty", file.string(), e.what());
				return 0;
			}
			std::size_t added = 0;
			{
				auto&            state = State();
				std::scoped_lock lock{ state.lock };
				for (const auto& [key, entry] : entries) added += state.entries.emplace(key, entry).second;
			}
			REX::DEBUG("[UI] loaded {} cached clip duration(s) from {}", added, file.string());
			return added;
		}

		void SaveCache()
		{
			auto&                                  state = State();
			std::unordered_map<std::string, Entry> entries;
			std::uint64_t                          revision;
			{
				std::scoped_lock lock{ state.lock };
				if (state.revision == state.savedRevision)
					return;
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
					REX::WARN("[UI] cannot write clip-duration cache {} — values won't persist this session", tmp.string());
					return;
				}
				// Keys are ACP-encoded paths, not necessarily valid UTF-8.
				out << doc.dump(1, '\t', false, json::error_handler_t::replace);
				out.close();
				if (!out) {
					REX::WARN("[UI] cannot finish clip-duration cache {} — keeping pending values", tmp.string());
					return;
				}
			}
			std::filesystem::rename(tmp, file, ec);
			if (ec) {
				REX::WARN("[UI] cannot replace clip-duration cache {} ({})", file.string(), ec.message());
				return;
			}
			std::scoped_lock lock{ state.lock };
			state.savedRevision = revision;  // New records remain dirty if playback ran during the write.
		}

		void MergeDecoded()
		{
			json result;
			if (!API::EngineClient::Get().ReadClipDurations(API::EngineClient::ReadJson, &result) || !result.is_object())
				return;
			const auto       decoded = result.get<Catalog::DecodedDurations>();
			auto&            state = State();
			std::scoped_lock lock{ state.lock };
			for (const auto& [key, value] : decoded) {
				if (!Plausible(value.sec) || g_state.decodedRevisions[key] >= value.revision)
					continue;
				g_state.decodedRevisions[key] = value.revision;
				auto& entry = state.entries[key];
				entry.sec = value.sec;
				entry.exact = true;
				++state.revision;
			}
		}
		void Probe(const Catalog::Clip& clip)
		{
			API::ClipProbeResult previous;
			bool                 known = false;
			std::uint64_t        decodedRevision{};
			auto&                state = State();
			{
				std::scoped_lock lock{ state.lock };
				decodedRevision = g_state.decodedRevisions[clip.key];
				if (const auto it = state.entries.find(clip.key); it != state.entries.end()) {
					previous.bytes = it->second.size;
					previous.modified = it->second.mtime;
					known = previous.bytes != 0;
				}
			}
			API::ClipProbeResult result;
			if (!API::EngineClient::Get().ProbeClipDuration(clip.file.c_str(), clip.animId.c_str(), known ? &previous : nullptr, &result))
				return;
			std::scoped_lock lock{ state.lock };
			if (g_state.decodedRevisions[clip.key] != decodedRevision)
				return;
			if (result.status == API::ClipProbeStatus::Unprobeable) {
				if (state.entries.erase(clip.key))
					++state.revision;
			} else if (result.status == API::ClipProbeStatus::Available && Plausible(result.seconds)) {
				auto& entry = state.entries[clip.key];
				if (!entry.exact || std::abs(entry.sec - result.seconds) > 0.05f) {
					entry.sec = result.seconds;
					entry.exact = false;
				}
				entry.size = result.bytes;
				entry.mtime = result.modified;
				++state.revision;
			}
		}
		void ScanPass(const std::vector<Catalog::Clip>& clips)
		{
			LoadCache();
			MergeDecoded();
			std::unordered_set<std::string> seen;
			for (const auto& clip : clips) {
				if (!seen.insert(clip.key).second)
					continue;
				try {
					Probe(clip);
				} catch (...) {
					REX::WARN("[UI] Could not probe clip duration for '{}'", clip.file);
				}
			}
			// Re-read after IO: an older probe/cache must never win over a concurrent playback decode.
			MergeDecoded();
			{
				auto&            state = State();
				std::scoped_lock lock{ state.lock };
				for (auto it = state.entries.begin(); it != state.entries.end();) {
					if (!it->second.exact && !seen.contains(it->first)) {
						it = state.entries.erase(it);
						++state.revision;
					} else
						++it;
				}
			}
			SaveCache();
			REX::DEBUG("[UI] Duration scan finished for {} unique clips", seen.size());
		}
	}
	std::optional<float> Lookup(std::string_view key)
	{
		auto&            state = State();
		std::scoped_lock lock{ state.lock };
		const auto       it = state.entries.find(std::string{ key });
		return it == state.entries.end() ? std::nullopt : std::optional{ it->second.sec };
	}
	void RefreshDecoded() { MergeDecoded(); }
	void Scan(std::vector<Catalog::Clip> clips, std::function<void()> onChanged)
	{
		{
			std::scoped_lock lock{ g_state.lock };
			g_state.pending = std::move(clips);
			if (g_state.running)
				return;
			g_state.running = true;
		}
		try {
			std::thread([onChanged = std::move(onChanged)] {
				for (;;) {
					std::vector<Catalog::Clip> clips;
					{
						std::scoped_lock lock{ g_state.lock };
						clips = std::move(*g_state.pending);
						g_state.pending.reset();
					}
					try {
						ScanPass(clips);
					} catch (...) {
						REX::ERROR("[UI] Clip-duration scan failed");
					}
					std::scoped_lock lock{ g_state.lock };
					if (!g_state.pending) {
						g_state.running = false;
						break;
					}
				}
				API::EngineClient::CatalogData::GetSingleton().Invalidate();
				if (onChanged)
					API::EngineClient::Post(std::move(onChanged));
			}).detach();
		} catch (...) {
			std::scoped_lock lock{ g_state.lock };
			g_state.running = false;
			g_state.pending.reset();
			REX::ERROR("[UI] Could not start clip-duration scan");
		}
	}
}
