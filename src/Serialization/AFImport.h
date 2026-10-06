#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Animation/OzzTypes.h"

namespace OSF::Serialization
{
	enum class AFError : uint8_t
	{
		kSuccess = 0,
		kAfReadFailed,
		kAfParseFailed,
		kRigReadFailed,
		kRigParseFailed,
		kFailedToBuildSkeleton,
		kFailedToMakeClip
	};

	struct AFLoadResult
	{
		AFError     error = AFError::kAfReadFailed;
		std::string detail;
		std::shared_ptr<const Animation::OzzSkeleton> skeleton;
		std::shared_ptr<const Animation::OzzAnimation> anim;
	};

	// Imports an engine-native `.af` clip + its skeleton.rig into ozz (decode ported from CALUMI.Animation).
	// `.af` tracks are quantized and rest-relative, mapped positionally onto the first `boneCountAnimated` rig bones.
	class AFImport
	{
	public:
		// `.af` carries no explicit rate; 30 fps is the Creation Engine convention.
		static constexpr float kAfFps = 30.0f;

		// Results are cached process-wide, keyed on (af, rig); the parsed rig + skeleton are shared.
		static AFLoadResult LoadAnimation(const std::filesystem::path& a_afFile, const std::filesystem::path& a_rigFile);

		// Called only on a rig-cache miss; nullopt = rig unavailable.
		using RigBytesProvider = std::function<std::optional<std::vector<std::uint8_t>>()>;

		static AFLoadResult LoadAnimation(const std::filesystem::path& a_afFile, std::string_view a_rigKey,
			const RigBytesProvider& a_rigProvider);

		// `a_clipKey` is the cache/log identity for caller-supplied bytes.
		static AFLoadResult LoadAnimation(std::string_view a_clipKey, const std::vector<std::uint8_t>& a_afBytes,
			std::string_view a_rigKey, const RigBytesProvider& a_rigProvider);

		static void ClearCache();
	};
}
