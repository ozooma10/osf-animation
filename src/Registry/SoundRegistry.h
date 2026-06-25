#pragma once

// Loads tagged sound pools from Data/OSF/**/*.sounds.json and resolves a "$tag,tag,..." pool reference to a clip by weighted-random pick. 

#include <cstdint>
#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace OSF::Registry
{
	// Sound-pool schema version (*.sounds.json declares this). Bump only on a breaking change.
	inline constexpr std::int64_t kSoundSchemaVersion = 1;

	// One clip in a pool: a Data-relative file path (or an "event:<name>" Wwise spec) plus a selection weight. weight is clamped to [1, 1000000] at load.
	struct SoundClip
	{
		std::string  spec;        // Data-relative path or "event:<name>"
		std::int32_t weight = 1;  // weighted-random selection weight
	};

	// A tagged bag of interchangeable clips. A query whose every tag is present in `tags` (all-of)
	// contributes ALL of this pool's clips to the candidate set. `name` is for diagnostics only.
	struct SoundPool
	{
		std::string              name;   // optional, "" if unnamed
		std::vector<std::string> tags;   // lowercased at load
		std::vector<SoundClip>   clips;
		std::filesystem::path    sourceFile;
	};

	class SoundRegistry
	{
	public:
		static SoundRegistry& GetSingleton();

		// Scans Data/OSF/**/*.sounds.json and rebuilds the pool set. Bad files/pools are skipped;
		void LoadAll();

		// Resolve a pool reference to a clip spec, or nullopt if nothing matches.
		// a_ref is a '$'-prefixed, comma-separated all-of tag list ("$seduce,female,moan,loud"); a leading '$' is optional (a raw "seduce,female" also works). 
		// Whitespace is trimmed, tags are lowercased. Picks weighted-random across the union of all matching pools' clips, avoiding an immediate repeat of the last clip when more than one candidate exists.
		std::optional<std::string> Resolve(std::string_view a_ref) const;

		// Lower-level pick by an explicit, already-split all-of tag set. Exposed for tests.
		std::optional<std::string> Pick(const std::vector<std::string>& a_allOf) const;

		// Number of loaded pools.
		std::size_t Size() const;

		// Problems (errors + warnings) from the last LoadAll, for diagnostics.
		std::vector<std::string> LoadErrors() const;

	private:
		mutable std::shared_mutex lock;
		std::vector<SoundPool>    pools;
		std::vector<std::string>  loadErrors;
		mutable std::string       lastPick;  // anti-repeat memory (guarded by `lock`)
	};
}
