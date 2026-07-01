#include "Util/Gzip.h"

#include <zlib.h>

namespace OSF::Util
{
	std::optional<std::vector<std::byte>> MaybeGunzip(std::vector<std::byte> a_buffer)
	{
		const bool isGzip = a_buffer.size() >= 2 &&
			a_buffer[0] == std::byte{ 0x1F } && a_buffer[1] == std::byte{ 0x8B };
		if (!isGzip) {
			return a_buffer;
		}

		z_stream strm{};
		// 15 = max window, +16 = expect a gzip header
		if (inflateInit2(&strm, 15 + 16) != Z_OK) {
			return std::nullopt;
		}

		std::vector<std::byte> out;
		std::byte chunk[65536];
		strm.next_in = reinterpret_cast<Bytef*>(a_buffer.data());
		strm.avail_in = static_cast<uInt>(a_buffer.size());

		int ret = Z_OK;
		do {
			strm.next_out = reinterpret_cast<Bytef*>(chunk);
			strm.avail_out = sizeof(chunk);
			ret = inflate(&strm, Z_NO_FLUSH);
			if (ret != Z_OK && ret != Z_STREAM_END) {
				inflateEnd(&strm);
				return std::nullopt;
			}
			out.insert(out.end(), chunk, chunk + (sizeof(chunk) - strm.avail_out));
		} while (ret != Z_STREAM_END);

		inflateEnd(&strm);
		return out;
	}
}
