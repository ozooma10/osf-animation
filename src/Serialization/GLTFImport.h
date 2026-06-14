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
		// Loads a .glb/.gltf file (transparently gunzipping if needed), builds an
		// ozz skeleton from its node hierarchy and compiles the selected animation
		// (by name, or by index if a_animId is numeric; empty selects the first
		// animation) into an ozz runtime animation.
		//
		// Successful results are memoized in a process-wide cache keyed on
		// (normalized path, animId): the ozz data is immutable behind
		// shared_ptr<const>, so a replay or a multi-stage preload of the same
		// file costs one gunzip+parse+build per session instead of one per use.
		// The cache holds strong references (bounded by the distinct clips
		// actually played in a session) — ClearCache() drops it.
		static LoadResult LoadAnimation(const std::filesystem::path& a_file, std::string_view a_animId);

		// Drops the clip cache. Called by OSF.ReloadPacks — the dev edit loop
		// where GLBs may have changed on disk under the same path.
		static void ClearCache();

	private:
		static LoadResult LoadAnimationUncached(const std::filesystem::path& a_file, std::string_view a_animId);
	};
}
