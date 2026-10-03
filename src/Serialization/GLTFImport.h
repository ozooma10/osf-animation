#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Animation/OzzTypes.h"

namespace OSF::Serialization
{
	enum class GLTFError : uint8_t
	{
		kSuccess = 0,
		kFileReadFailed,      // includes gzip failure
		kParseFailed,
		kNoAnimations,
		kInvalidAnimationIdentifier,
		kFailedToBuildSkeleton,
		kFailedToMakeClip
	};

	struct LoadResult
	{
		GLTFError error = GLTFError::kFileReadFailed;
		std::string detail;
		std::shared_ptr<const Animation::OzzSkeleton> skeleton;
		std::shared_ptr<const Animation::OzzAnimation> anim;
	};

	class GLTFImport
	{
	public:
		// a_animId: name, numeric index, or empty for the first animation.
		// Successes are cached process-wide, keyed on (normalized path, animId).
		static LoadResult LoadAnimation(const std::filesystem::path& a_file, std::string_view a_animId);

		// `a_parentPath` resolves external buffers of a text .gltf.
		static LoadResult LoadAnimation(std::string_view a_cacheKey, std::vector<std::byte> a_bytes,
			const std::filesystem::path& a_parentPath, std::string_view a_animId);

		static void ClearCache();

	private:
		static LoadResult LoadAnimationUncached(const std::filesystem::path& a_file, std::string_view a_animId);
		static LoadResult LoadAnimationUncached(std::vector<std::byte> a_bytes,
			const std::filesystem::path& a_parentPath, std::string_view a_animId);
	};
}
