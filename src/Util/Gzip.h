#pragma once

// Transparent gzip decompression for clip buffers (NAF-convention .glb files are often gzipped
// on disk under the same extension). Shared by the GLTF importer and the clip-duration probe.

#include <cstddef>
#include <optional>
#include <vector>

namespace OSF::Util
{
	// If the buffer is gzip-compressed, decompresses it; a non-gzip buffer passes through
	// unchanged. nullopt = the buffer claimed gzip but failed to decompress.
	std::optional<std::vector<std::byte>> MaybeGunzip(std::vector<std::byte> a_buffer);
}
