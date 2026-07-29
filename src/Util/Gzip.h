#pragma once

// Transparent gzip decompression for clip buffers (NAF-convention .glb files are often gzipped
// on disk under the same extension). Shared by the GLTF importer and the clip-duration probe.

#include <cstddef>
#include <optional>
#include <vector>

namespace OSF::Util
{
	// One shared ceiling for any clip buffer read from disk (raw file bytes AND gunzip output —
	// capping only the compressed side would leave a gzip bomb able to OOM the game process).
	// Far above any real clip (vanilla .af and NAF .glb clips are a few MB), far below harm.
	inline constexpr std::size_t kMaxClipBytes = 256ull * 1024 * 1024;

	// If the buffer is gzip-compressed, decompresses it; a non-gzip buffer passes through
	// unchanged. nullopt = the buffer claimed gzip but failed to decompress, or the
	// decompressed size exceeded kMaxClipBytes.
	std::optional<std::vector<std::byte>> MaybeGunzip(std::vector<std::byte> a_buffer);
}
