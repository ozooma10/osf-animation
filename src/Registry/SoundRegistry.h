#pragma once

// Tagged sound pools from Data/OSF/**/*.sounds.json; a "$tag,tag" ref resolves to a clip by weighted-random pick.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OSF::Registry
{
	inline constexpr std::int64_t kSoundSchemaVersion = 1;

	struct SoundClip
	{
		std::string  spec;        // Data-relative path or "event:<name>"
		std::int32_t weight = 1;  // clamped to [1, 1000000] at load
		std::string  text;        // subtitle shown when played; "" = none
	};

	// A query matches when every query tag is in `tags` (all-of); all matching pools' clips are candidates.
	struct SoundPool
	{
		std::string              name;   // diagnostics only; "" if unnamed
		std::vector<std::string> tags;   // lowercased at load
		std::vector<SoundClip>   clips;
		std::filesystem::path    sourceFile;
	};

	struct SoundFileStats
	{
		std::string              file;  // file name only; empty for cross-file/discovery problems
		std::string              path;  // slash-delimited, relative to Data/OSF
		std::uint32_t            errors = 0;
		std::uint32_t            warnings = 0;
		std::vector<std::string> problems;
	};

	class SoundRegistry
	{
	public:
		static SoundRegistry& GetSingleton();

		// Bad files/pools are skipped and recorded, not fatal.
		void LoadAll();

		// a_ref: optional '$' + comma-separated all-of tags ("$seduce,female,moan"). Avoids repeating the last pick.
		std::optional<std::string> Resolve(std::string_view a_ref) const;

		// Pre-split all-of tag set; exposed for tests.
		std::optional<std::string> Pick(const std::vector<std::string>& a_allOf) const;

		// Subtitle for a resolved clip spec, or "" if none.
		std::string TextForClip(std::string_view a_spec) const;

		std::size_t Size() const;

		std::vector<std::string> LoadErrors() const;

		std::vector<SoundFileStats> FileStats() const;

	private:
		mutable std::shared_mutex        lock;
		std::vector<SoundPool>           pools;
		std::unordered_map<std::string, std::string> clipText;  // spec -> subtitle; first-wins on dup
		std::vector<std::string>          loadErrors;
		std::vector<SoundFileStats>       fileStats;
		mutable std::string               lastPick;  // anti-repeat; guarded by `lock`
	};
}
