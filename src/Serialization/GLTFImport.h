#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "Animation/OzzTypes.h"

namespace OSF::Serialization
{
	enum class GLTFError : uint8_t
	{
		kSuccess = 0,
		kFileReadFailed,      // missing/unreadable file (incl. gzip decompress failure)
		kParseFailed,         // fastgltf rejected the data
		kNoAnimations,
		kInvalidAnimationIdentifier,
		kFailedToBuildSkeleton,
		kFailedToMakeClip
	};

	struct LoadResult
	{
		GLTFError error = GLTFError::kFileReadFailed;
		std::string detail;  // human-readable diagnostics for logging
		std::shared_ptr<const Animation::OzzSkeleton> skeleton;
		std::shared_ptr<const Animation::OzzAnimation> anim;
	};

	class GLTFImport
	{
	public:
		// Loads a .glb/.gltf file, gunzipping it first if needed, builds an ozz skeleton from
		// its node hierarchy, and compiles the chosen animation into an ozz runtime animation.
		// The animation is picked by name, or by index if a_animId is numeric; an empty id
		// selects the first one.
		//
		// Successful results are cached process-wide, keyed on (normalized path, animId). The
		// ozz data is immutable behind shared_ptr<const>, so replaying or pre-loading the same
		// file across stages costs one gunzip+parse+build per session rather than one per use.
		// The cache keeps strong references, bounded by however many distinct clips actually
		// play in a session; ClearCache() drops them all.
		static LoadResult LoadAnimation(const std::filesystem::path& a_file, std::string_view a_animId);

		// Drops the clip cache. Called by OSF.ReloadPacks — the dev edit loop
		// where GLBs may have changed on disk under the same path.
		static void ClearCache();

	private:
		static LoadResult LoadAnimationUncached(const std::filesystem::path& a_file, std::string_view a_animId);
	};
}
