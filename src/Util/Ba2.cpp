#include "Util/Ba2.h"

#include "Util/StringUtil.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <zlib.h>

namespace OSF::Util::Ba2
{
	namespace
	{
		// Lowercase + backslashes -> forward slashes, for archive-name matching.
		std::string Normalize(std::string_view a_path)
		{
			std::string s = ToLower(a_path);
			for (auto& c : s) {
				if (c == '\\') {
					c = '/';
				}
			}
			return s;
		}

		template <class T>
		bool ReadAt(std::ifstream& a_f, std::uint64_t a_pos, T& a_out)
		{
			a_f.seekg(static_cast<std::streamoff>(a_pos));
			return static_cast<bool>(a_f.read(reinterpret_cast<char*>(&a_out), sizeof(T)));
		}

		// Looks for a_want inside one GNRL archive; returns its decompressed bytes if present.
		std::optional<std::vector<std::uint8_t>> ReadFromArchive(const std::filesystem::path& a_archive, const std::string& a_want)
		{
			std::ifstream f(a_archive, std::ios::binary);
			if (!f) {
				return std::nullopt;
			}

			char     magic[4]{};
			char     type[4]{};
			std::uint32_t version = 0;
			f.read(magic, 4);
			if (!f || std::memcmp(magic, "BTDX", 4) != 0) {
				return std::nullopt;
			}
			ReadAt(f, 4, version);
			f.seekg(8);
			f.read(type, 4);
			if (!f || std::memcmp(type, "GNRL", 4) != 0) {
				return std::nullopt;  // texture (DX10) or other archive — not handled
			}

			std::uint32_t fileCount = 0;
			std::uint64_t nameTableOffset = 0;
			if (!ReadAt(f, 12, fileCount) || !ReadAt(f, 16, nameTableOffset) || fileCount == 0) {
				return std::nullopt;
			}

			// Name table: per file, u16 length + raw name bytes. Walk it to find a_want's index.
			f.seekg(static_cast<std::streamoff>(nameTableOffset));
			std::vector<char> nameBlob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			std::size_t found = static_cast<std::size_t>(-1);
			{
				std::size_t p = 0;
				for (std::uint32_t i = 0; i < fileCount; i++) {
					if (p + 2 > nameBlob.size()) {
						break;
					}
					std::uint16_t nlen = 0;
					std::memcpy(&nlen, nameBlob.data() + p, 2);
					p += 2;
					if (p + nlen > nameBlob.size()) {
						break;
					}
					if (Normalize({ nameBlob.data() + p, nlen }) == a_want) {
						found = i;
						break;
					}
					p += nlen;
				}
			}
			if (found == static_cast<std::size_t>(-1)) {
				return std::nullopt;
			}

			// Record (36 bytes) for `found`. Starfield v2 records start at 0x20.
			const std::uint64_t recOff = (version >= 2 ? 0x20 : 0x18) + static_cast<std::uint64_t>(found) * 36;
			std::uint64_t dataOffset = 0;
			std::uint32_t packed = 0;
			std::uint32_t unpacked = 0;
			if (!ReadAt(f, recOff + 16, dataOffset) || !ReadAt(f, recOff + 24, packed) || !ReadAt(f, recOff + 28, unpacked)) {
				return std::nullopt;
			}
			if (unpacked == 0) {
				return std::vector<std::uint8_t>{};
			}

			const std::uint32_t storedSize = packed != 0 ? packed : unpacked;

			// Reject absurd records rather than allocating gigabytes from a corrupt or foreign archive 
			constexpr std::uint32_t kMaxReasonable = 64u * 1024 * 1024;  // 64 MiB ceiling
			if (unpacked > kMaxReasonable || packed > kMaxReasonable) {
				return std::nullopt;
			}
			std::error_code sizeEc;
			const auto fileSize = std::filesystem::file_size(a_archive, sizeEc);
			if (!sizeEc && dataOffset + storedSize > fileSize) {
				return std::nullopt;
			}

			std::vector<std::uint8_t> stored(storedSize);
			f.seekg(static_cast<std::streamoff>(dataOffset));
			if (!f.read(reinterpret_cast<char*>(stored.data()), storedSize)) {
				return std::nullopt;
			}
			if (packed == 0) {
				return stored;  // stored uncompressed
			}

			std::vector<std::uint8_t> out(unpacked);
			uLongf destLen = unpacked;
			if (uncompress(out.data(), &destLen, stored.data(), storedSize) != Z_OK || destLen != unpacked) {
				return std::nullopt;
			}
			return out;
		}
	}

	std::optional<std::vector<std::uint8_t>> ReadGameFile(std::string_view a_relPath, std::string_view a_preferredArchive)
	{
		const std::string want = Normalize(a_relPath);
		std::error_code ec;
		const auto dataDir = std::filesystem::current_path() / "Data";
		if (!std::filesystem::is_directory(dataDir, ec)) {
			return std::nullopt;
		}

		// Fast path: a caller that knows which base archive holds the asset skips the directory walk straight to it.
		if (!a_preferredArchive.empty()) {
			if (auto bytes = ReadFromArchive(dataDir / a_preferredArchive, want)) {
				return bytes;
			}
			// Not there — fall through to a full scan (covers archive renames/splits in game updates).
		}

		for (const auto& entry : std::filesystem::directory_iterator(dataDir, ec)) {
			if (ec) {
				break;
			}
			if (!entry.is_regular_file(ec) || ToLower(entry.path().extension().string()) != ".ba2") {
				continue;
			}
			if (auto bytes = ReadFromArchive(entry.path(), want)) {
				return bytes;  // exact path is unique to one archive (the base human rig is in Starfield - Animations.ba2)
			}
		}
		return std::nullopt;
	}
}
