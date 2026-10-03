#pragma once

// NAF-convention .glb clips are often gzipped under the same extension.

#include <cstddef>
#include <optional>
#include <vector>

namespace OSF::Util
{
	// Caps raw reads and gunzip output alike, so a gzip bomb can't OOM the game.
	inline constexpr std::size_t kMaxClipBytes = 256ull * 1024 * 1024;

	// Non-gzip passes through; nullopt on a bad stream or output over kMaxClipBytes.
	std::optional<std::vector<std::byte>> MaybeGunzip(std::vector<std::byte> a_buffer);
}
