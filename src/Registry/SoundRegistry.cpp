#include "Registry/SoundRegistry.h"

#include "Util/RegistryFiles.h"
#include "Util/StringUtil.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <random>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace OSF::Registry
{
	using OSF::Util::ToLower;

	namespace
	{
		using json = nlohmann::json;

		constexpr std::size_t kMaxPoolsPerFile = 2048;
		constexpr std::size_t kMaxTagsPerPool = 64;
		constexpr std::size_t kMaxClipsPerPool = 4096;
		constexpr std::size_t kMaxSoundPoolsTotal = 8192;
		constexpr std::size_t kMaxSoundClipsTotal = 131072;
		constexpr std::size_t kMaxSoundStringBytes = 16384;

		std::int32_t ClampWeight(std::int64_t a_w)
		{
			return static_cast<std::int32_t>(std::clamp<std::int64_t>(a_w, 1, 1000000));
		}

		// One clip in the ARRAY form: a bare "path" string (weight 1, no text) or a
		// { spec|file, weight?, text? } object. `text` is the optional subtitle shown when it plays.
		SoundClip ParseClip(const json& a_clip, const std::string& a_subject)
		{
			SoundClip clip;
			if (a_clip.is_string()) {
				clip.spec = a_clip.get<std::string>();
			} else if (a_clip.is_object()) {
				if (auto sit = a_clip.find("spec"); sit != a_clip.end()) {
					clip.spec = sit->get<std::string>();
				} else if (auto fit = a_clip.find("file"); fit != a_clip.end()) {
					clip.spec = fit->get<std::string>();
				}
				clip.weight = ClampWeight(a_clip.value("weight", std::int64_t{ 1 }));
				clip.text = a_clip.value("text", std::string{});  // optional subtitle line
			} else {
				throw std::runtime_error(a_subject + ": a clip must be a path string or a { spec/file, weight, text } object");
			}
			if (clip.spec.empty()) {
				throw std::runtime_error(a_subject + ": empty clip spec");
			}
			if (clip.spec.size() > kMaxSoundStringBytes || clip.text.size() > kMaxSoundStringBytes) {
				throw std::runtime_error(a_subject + ": clip spec/text exceeds the string-size limit");
			}
			return clip;
		}

		SoundPool ParsePool(const json& a_pool, const std::filesystem::path& a_file)
		{
			SoundPool pool;
			pool.name = a_pool.value("name", std::string{});
			if (pool.name.size() > kMaxSoundStringBytes) {
				throw std::runtime_error("pool name exceeds the string-size limit");
			}
			pool.sourceFile = a_file;
			const std::string subject = pool.name.empty() ? std::string("pool") : ("pool '" + pool.name + "'");

			const auto tit = a_pool.find("tags");
			if (tit == a_pool.end() || !tit->is_array() || tit->empty()) {
				throw std::runtime_error(subject + ": needs a non-empty 'tags' array");
			}
			if (tit->size() > kMaxTagsPerPool) {
				throw std::runtime_error(subject + ": too many tags");
			}
			for (const auto& t : *tit) {
				if (!t.is_string()) {
					throw std::runtime_error(subject + ": tag entries must be strings");
				}
				auto tag = t.get<std::string>();
				if (tag.size() > kMaxSoundStringBytes) {
					throw std::runtime_error(subject + ": tag exceeds the string-size limit");
				}
				pool.tags.push_back(ToLower(tag));
			}

			// `clips` is EITHER an array (paths / { spec, weight, text } objects) OR a terse object
			// mapping each clip path -> its spoken text ({ "Sound/.../x.wav": "the line", ... }), the
			// shorthand for "give these clips subtitles". Object-form clips are weight 1.
			const auto cit = a_pool.find("clips");
			if (cit == a_pool.end()) {
				throw std::runtime_error(subject + ": needs 'clips' (a non-empty array, or a { path: text } object)");
			}
			if (cit->is_object()) {
				if (cit->empty()) {
					throw std::runtime_error(subject + ": 'clips' object is empty");
				}
				if (cit->size() > kMaxClipsPerPool) {
					throw std::runtime_error(subject + ": too many clips");
				}
				for (auto it = cit->begin(); it != cit->end(); ++it) {
					SoundClip clip;
					clip.spec = it.key();
					if (clip.spec.empty()) {
						throw std::runtime_error(subject + ": a 'clips' entry has an empty path key");
					}
					// value = the subtitle text; null / "" means "this clip, no subtitle".
					if (it.value().is_string()) {
						clip.text = it.value().get<std::string>();
					} else if (!it.value().is_null()) {
						throw std::runtime_error(subject + ": clip '" + clip.spec + "': value must be a subtitle string (or null)");
					}
					if (clip.spec.size() > kMaxSoundStringBytes || clip.text.size() > kMaxSoundStringBytes) {
						throw std::runtime_error(subject + ": clip spec/text exceeds the string-size limit");
					}
					pool.clips.push_back(std::move(clip));
				}
			} else if (cit->is_array()) {
				if (cit->empty()) {
					throw std::runtime_error(subject + ": 'clips' array is empty");
				}
				if (cit->size() > kMaxClipsPerPool) {
					throw std::runtime_error(subject + ": too many clips");
				}
				for (const auto& c : *cit) {
					pool.clips.push_back(ParseClip(c, subject));
				}
			} else {
				throw std::runtime_error(subject + ": 'clips' must be a non-empty array or a { path: text } object");
			}
			return pool;
		}

		// A file is { schema, pools: [...] }. Bad pools are skipped (recorded), not fatal to the file.
		void LoadSoundFile(const json& a_json, const std::filesystem::path& a_file,
			std::vector<SoundPool>& a_out, std::size_t& a_clipCount,
			std::vector<std::string>& a_errors)
		{
			const std::string fileName = a_file.filename().string();

			const auto sit = a_json.find("schema");
			if (sit == a_json.end() || !sit->is_number_integer()) {
				a_errors.push_back("[error] '" + fileName + "': missing/non-integer 'schema'");
				REX::ERROR("[Sound] '{}' missing/non-integer 'schema' — skipped", fileName);
				return;
			}
			const auto schema = sit->get<std::int64_t>();
			if (schema != kSoundSchemaVersion) {
				a_errors.push_back("[error] '" + fileName + "': *.sounds.json schema " + std::to_string(schema) +
					" unsupported (expected " + std::to_string(kSoundSchemaVersion) + ")");
				REX::ERROR("[Sound] '{}' declares sound schema {} but this build expects {} — skipped",
					fileName, schema, kSoundSchemaVersion);
				return;
			}

			const auto pit = a_json.find("pools");
			if (pit == a_json.end() || !pit->is_array()) {
				a_errors.push_back("[error] '" + fileName + "': needs a 'pools' array");
				REX::ERROR("[Sound] '{}' needs a 'pools' array — skipped", fileName);
				return;
			}
			if (pit->size() > kMaxPoolsPerFile) {
				a_errors.push_back("[error] '" + fileName + "': too many pools");
				REX::ERROR("[Sound] '{}' contains more than {} pools — skipped", fileName, kMaxPoolsPerFile);
				return;
			}
			for (const auto& jp : *pit) {
				if (a_out.size() >= kMaxSoundPoolsTotal || a_clipCount >= kMaxSoundClipsTotal) {
					a_errors.push_back("[error] sound registry reached its global pool/clip limit");
					REX::ERROR("[Sound] global pool/clip limit reached — remaining pools skipped");
					break;
				}
				try {
					auto pool = ParsePool(jp, a_file);
					if (pool.clips.size() > kMaxSoundClipsTotal - a_clipCount) {
						throw std::runtime_error("global sound clip limit would be exceeded");
					}
					const auto poolClipCount = pool.clips.size();
					REX::DEBUG("[Sound] loaded pool '{}' ({} clip(s), {} tag(s)) from '{}'",
						pool.name.empty() ? "<unnamed>" : pool.name, poolClipCount, pool.tags.size(), fileName);
					a_out.push_back(std::move(pool));
					a_clipCount += poolClipCount;
				} catch (const std::exception& e) {
					a_errors.push_back("[error] '" + fileName + "': " + e.what());
					REX::ERROR("[Sound] skipping pool in '{}': {}", fileName, e.what());
				}
			}
		}
	}

	SoundRegistry& SoundRegistry::GetSingleton()
	{
		static SoundRegistry singleton;
		return singleton;
	}

	void SoundRegistry::LoadAll()
	{
		std::vector<SoundPool> loaded;
		std::vector<std::string> errors;
		std::vector<SoundFileStats> stats;
		SoundFileStats crossFile;
		std::size_t loadedClipCount = 0;

		std::unordered_map<std::string, std::size_t> statsByFile;
		const auto ensureStats = [&](const Util::RegistryJsonSource& a_source) -> SoundFileStats& {
			if (const auto found = statsByFile.find(a_source.file.string()); found != statsByFile.end()) {
				return stats[found->second];
			}
			const auto relative = a_source.file.lexically_relative(a_source.root);
			SoundFileStats record{ .file = a_source.file.filename().string(),
				.path = relative.empty() ? a_source.file.filename().generic_string() : relative.generic_string() };
			statsByFile.emplace(a_source.file.string(), stats.size());
			return stats.emplace_back(std::move(record));
		};

		Util::ForEachRegistryJson(".sounds.json",
			[](std::ifstream& a_input) {
				return nlohmann::json::parse(a_input, nullptr, true, true);
			},
			[&](const Util::RegistryJsonSource& a_source, const nlohmann::json& a_json) {
				auto& record = ensureStats(a_source);
				const auto before = errors.size();
				LoadSoundFile(a_json, a_source.file, loaded, loadedClipCount, errors);
				for (std::size_t i = before; i < errors.size(); ++i) {
					(errors[i].starts_with("[warn]") ? record.warnings : record.errors) += 1;
					record.problems.push_back(errors[i]);
				}
			},
			[&](Util::RegistryJsonProblemKind a_kind, const Util::RegistryJsonSource* a_source,
				const std::string& a_message) {
				if (a_kind == Util::RegistryJsonProblemKind::kFile && a_source) {
					auto& record = ensureStats(*a_source);
					const bool unknown = a_message == "unknown exception";
					errors.push_back("[error] '" + record.file + "': " +
						(unknown ? "parse failed with an unknown exception" : "parse failed: " + a_message));
					++record.errors;
					record.problems.push_back(errors.back());
					if (unknown) {
						REX::ERROR("[Sound] failed to parse '{}' with an unknown exception", record.file);
					} else {
						REX::ERROR("[Sound] failed to parse '{}': {}", record.file, a_message);
					}
					return;
				}
				const bool directory = a_kind == Util::RegistryJsonProblemKind::kGameDirectory;
				errors.push_back("[error] " + std::string(directory ? "" : "sound discovery: ") + a_message);
				++crossFile.errors;
				crossFile.problems.push_back(errors.back());
				if (directory) {
					REX::ERROR("[Sound] {}", a_message);
				} else {
					REX::ERROR("[Sound] discovery: {}", a_message);
				}
			});

		if (!crossFile.problems.empty()) {
			stats.push_back(std::move(crossFile));
		}

		// Flatten clip spec -> subtitle text across every pool (first-wins on a duplicate path) so a
		// played clip can render its line by spec, regardless of which pool it came from.
		std::unordered_map<std::string, std::string> textMap;
		for (const auto& p : loaded) {
			for (const auto& c : p.clips) {
				if (!c.text.empty()) {
					textMap.emplace(c.spec, c.text);
				}
			}
		}

		const auto poolCount = loaded.size();
		const auto problemCount = errors.size();
		const auto textCount = textMap.size();
		{
			std::unique_lock l{ lock };
			pools = std::move(loaded);
			clipText = std::move(textMap);
			loadErrors = std::move(errors);
			fileStats = std::move(stats);
			lastPick.clear();
		}
		REX::INFO("[Sound] {} pool(s) loaded, {} subtitled clip(s), {} problem(s)", poolCount, textCount, problemCount);
	}
	std::optional<std::string> SoundRegistry::Resolve(std::string_view a_ref) const
	{
		// Strip the optional leading '$', split on ',', trim + lowercase each tag.
		std::string s(a_ref);
		if (!s.empty() && s.front() == '$') {
			s.erase(s.begin());
		}
		std::vector<std::string> tags;
		std::size_t start = 0;
		while (true) {
			const auto comma = s.find(',', start);
			const auto end = (comma == std::string::npos) ? s.size() : comma;
			std::size_t a = start;
			std::size_t b = end;
			while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) {
				++a;
			}
			while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) {
				--b;
			}
			if (b > a) {
				tags.push_back(ToLower(s.substr(a, b - a)));
			}
			if (comma == std::string::npos) {
				break;
			}
			start = comma + 1;
		}
		if (tags.empty()) {
			return std::nullopt;
		}
		return Pick(tags);
	}

	std::optional<std::string> SoundRegistry::Pick(const std::vector<std::string>& a_allOf) const
	{
		// Lowercase the query (idempotent when Resolve already lowered it; robust for direct callers).
		std::vector<std::string> query;
		query.reserve(a_allOf.size());
		for (const auto& t : a_allOf) {
			query.push_back(ToLower(t));
		}

		std::unique_lock l{ lock };

		// Union every matching pool's clips (all-of: each query tag must be present in the pool's tags).
		std::vector<const SoundClip*> candidates;
		std::uint64_t total = 0;
		for (const auto& p : pools) {
			bool matches = true;
			for (const auto& tag : query) {
				if (std::find(p.tags.begin(), p.tags.end(), tag) == p.tags.end()) {
					matches = false;
					break;
				}
			}
			if (!matches) {
				continue;
			}
			for (const auto& c : p.clips) {
				candidates.push_back(&c);
				total += static_cast<std::uint64_t>(std::max(1, c.weight));
			}
		}
		if (candidates.empty()) {
			return std::nullopt;
		}

		// Weight-proportional random (mirrors Matchmaker::Pick; uint64 sum is overflow-safe for the cap).
		static thread_local std::mt19937 rng{ std::random_device{}() };
		const auto rollOnce = [&]() -> const SoundClip* {
			std::uniform_int_distribution<std::uint64_t> dist(1, total);
			auto roll = dist(rng);
			for (const auto* c : candidates) {
				const auto w = static_cast<std::uint64_t>(std::max(1, c->weight));
				if (roll <= w) {
					return c;
				}
				roll -= w;
			}
			return candidates.back();  // numerical guard; not normally reached
		};

		const SoundClip* chosen = rollOnce();
		// Avoid an immediate repeat of the last clip when there's a real choice.
		if (chosen->spec == lastPick && candidates.size() > 1) {
			chosen = rollOnce();
		}
		lastPick = chosen->spec;
		return chosen->spec;
	}

	std::string SoundRegistry::TextForClip(std::string_view a_spec) const
	{
		std::shared_lock l{ lock };
		const auto it = clipText.find(std::string(a_spec));
		return it != clipText.end() ? it->second : std::string{};
	}

	std::size_t SoundRegistry::Size() const
	{
		std::shared_lock l{ lock };
		return pools.size();
	}

	std::vector<std::string> SoundRegistry::LoadErrors() const
	{
		std::shared_lock l{ lock };
		return loadErrors;
	}

	std::vector<SoundFileStats> SoundRegistry::FileStats() const
	{
		std::shared_lock l{ lock };
		return fileStats;
	}
}
