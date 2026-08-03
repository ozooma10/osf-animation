#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace OSF::Util
{
	struct ClipSpec;
}

namespace OSF::Animation
{
	struct OzzAnimation;
	struct OzzSkeleton;

	namespace GraphManagerClipLoad
	{
		// Internal loading boundary; GraphManager.h remains the public playback surface.
		struct Result
		{
			std::shared_ptr<const OzzSkeleton>  skeleton;
			std::shared_ptr<const OzzAnimation> anim;
			bool                                ok = false;
			std::string                         detail;
			std::string                         source;
		};

		Result Load(const Util::ClipSpec& a_spec, std::string_view a_animId);
		Result LoadAfBytes(std::string_view a_clipKey, const std::vector<std::uint8_t>& a_bytes);
	}
}
