#include "Registry/SoundRegistry.h"

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

		std::int32_t ClampWeight(std::int64_t a_w)
		{
			return static_cast<std::int32_t>(std::clamp<std::int64_t>(a_w, 1, 1000000));
		}

		// One clip: a bare "path" string (weight 1) or a { spec|file, weight } object.
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
			} else {
				throw std::runtime_error(a_subject + ": a clip must be a path string or a { spec/file, weight } object");
			}
			if (clip.spec.empty()) {
				throw std::runtime_error(a_subject + ": empty clip spec");
			}
			return clip;
		}

		SoundPool ParsePool(const json& a_pool, const std::filesystem::path& a_file)
		{
			SoundPool pool;
			pool.name = a_pool.value("name", std::string{});
			pool.sourceFile = a_file;
			const std::string subject = pool.name.empty() ? std::string("pool") : ("pool '" + pool.name + "'");

			const auto tit = a_pool.find("tags");
			if (tit == a_pool.end() || !tit->is_array() || tit->empty()) {
				throw std::runtime_error(subject + ": needs a non-empty 'tags' array");
			}
			for (const auto& t : *tit) {
				if (!t.is_string()) {
					throw std::runtime_error(subject + ": tag entries must be strings");
				}
				pool.tags.push_back(ToLower(t.get<std::string>()));
			}

			const auto cit = a_pool.find("clips");
			if (cit == a_pool.end() || !cit->is_array() || cit->empty()) {
				throw std::runtime_error(subject + ": needs a non-empty 'clips' array");
			}
			for (const auto& c : *cit) {
				pool.clips.push_back(ParseClip(c, subject));
			}
			return pool;
		}

		// A file is { schema, pools: [...] }. Bad pools are skipped (recorded), not fatal to the file.
		void LoadSoundFile(const json& a_json, const std::filesystem::path& a_file,
			std::vector<SoundPool>& a_out, std::vector<std::string>& a_errors)
		{
			const std::string fileName = a_file.filename().string();

			const auto sit = a_json.find("schema");
			if (sit == a_json.end() || !sit->is_number_integer()) {
				a_errors.push_back("[error] '" + fileName + "': missing/non-integer 'schema'");
				REX::ERROR("SoundRegistry: '{}' missing/non-integer 'schema' — skipped", fileName);
				return;
			}
			const auto schema = sit->get<std::int64_t>();
			if (schema != kSoundSchemaVersion) {
				a_errors.push_back("[error] '" + fileName + "': *.sounds.json schema " + std::to_string(schema) +
					" unsupported (expected " + std::to_string(kSoundSchemaVersion) + ")");
				REX::ERROR("SoundRegistry: '{}' declares sound schema {} but this build expects {} — skipped",
					fileName, schema, kSoundSchemaVersion);
				return;
			}

			const auto pit = a_json.find("pools");
			if (pit == a_json.end() || !pit->is_array()) {
				a_errors.push_back("[error] '" + fileName + "': needs a 'pools' array");
				REX::ERROR("SoundRegistry: '{}' needs a 'pools' array — skipped", fileName);
				return;
			}
			for (const auto& jp : *pit) {
				try {
					auto pool = ParsePool(jp, a_file);
					REX::INFO("SoundRegistry: loaded pool '{}' ({} clip(s), {} tag(s)) from '{}'",
						pool.name.empty() ? "<unnamed>" : pool.name, pool.clips.size(), pool.tags.size(), fileName);
					a_out.push_back(std::move(pool));
				} catch (const std::exception& e) {
					a_errors.push_back("[error] '" + fileName + "': " + e.what());
					REX::ERROR("SoundRegistry: skipping pool in '{}': {}", fileName, e.what());
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
		namespace fs = std::filesystem;
		std::vector<SoundPool> loaded;
		std::vector<std::string> errors;

		const fs::path dir = fs::current_path() / "Data" / "OSF";
		std::error_code ec;
		if (fs::is_directory(dir, ec)) {
			std::vector<fs::path> files;
			for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
				if (entry.is_regular_file(ec) && ToLower(entry.path().filename().string()).ends_with(".sounds.json")) {
					files.push_back(entry.path());
				}
			}
			// Deterministic load order (mirrors SceneRegistry).
			std::sort(files.begin(), files.end(), [](const fs::path& a_lhs, const fs::path& a_rhs) {
				return ToLower(a_lhs.filename().string()) < ToLower(a_rhs.filename().string());
			});
			for (const auto& file : files) {
				try {
					std::ifstream in(file, std::ios::binary);
					const auto j = nlohmann::json::parse(in, nullptr, true, true);  // tolerate // comments
					LoadSoundFile(j, file, loaded, errors);
				} catch (const std::exception& e) {
					errors.push_back("[error] '" + file.filename().string() + "': parse failed: " + e.what());
					REX::ERROR("SoundRegistry: failed to parse '{}': {}", file.filename().string(), e.what());
				}
			}
		}

		const auto poolCount = loaded.size();
		const auto problemCount = errors.size();
		{
			std::unique_lock l{ lock };
			pools = std::move(loaded);
			loadErrors = std::move(errors);
			lastPick.clear();
		}
		REX::INFO("SoundRegistry: {} pool(s) loaded, {} problem(s)", poolCount, problemCount);
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
		std::mt19937 rng{ std::random_device{}() };
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
}
