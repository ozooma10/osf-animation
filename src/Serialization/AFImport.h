#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "Animation/OzzTypes.h"

namespace OSF::Serialization
{
	enum class AFError : uint8_t
	{
		kSuccess = 0,
		kAfReadFailed,           // .af missing/unreadable
		kAfParseFailed,          // .af malformed (bad header / truncated / atlas mismatch)
		kRigReadFailed,          // skeleton.rig missing/unreadable
		kRigParseFailed,         // skeleton.rig malformed
		kFailedToBuildSkeleton,  // ozz skeleton build failed
		kFailedToMakeClip        // ozz animation build failed
	};

	struct AFLoadResult
	{
		AFError     error = AFError::kAfReadFailed;
		std::string detail;  // human-readable diagnostics for logging
		std::shared_ptr<const Animation::OzzSkeleton> skeleton;
		std::shared_ptr<const Animation::OzzAnimation> anim;
	};

	// Imports a Starfield engine-native `.af` clip into ozz, so it can play through the same sampler/stamper path as a GLB (Graph::Sample / StampPose)
	// OSF owns the clock, sync and scenes, sourced from `.af` content (including vanilla clips).
	//
	// The `.af` stores quantized, REST-RELATIVE per-bone tracks mapped POSITIONALLY onto the first `boneCountAnimated` bones of `skeleton.rig`. 
	// This decoder (ported from CALUMI.Animation, the authoritative reader): builds an ozz skeleton from the rig hierarchy + bind pose, 
	// dequantizes the tracks (smallest-three quaternion + rig-precision translation), re-bases them to absolute local-to-parent transforms,
	// and compiles an ozz runtime animation. Frame indices are mapped to time at kAfFps.
	class AFImport
	{
	public:
		// Frame rate used to convert `.af` frame indices to seconds. The `.af` carries no explicit rate; 30 fps is the Creation-Engine convention. 
		// Playback speed can be retuned via OSF.SetSpeed.
		static constexpr float kAfFps = 30.0f;

		// Loads `a_afFile` against `a_rigFile` (the human skeleton.rig). Successful results are cached  process-wide, keyed on (af path, rig path); 
		// the parsed rig + ozz skeleton are shared across every clip that uses the same rig.
		static AFLoadResult LoadAnimation(const std::filesystem::path& a_afFile, const std::filesystem::path& a_rigFile);

		// Drops the clip + rig caches (the OSF.ReloadPacks dev edit loop).
		static void ClearCache();
	};
}
