#pragma once

#include "Util/StringUtil.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OSF::Util
{
	inline constexpr std::uintmax_t kMaxRegistryFileBytes = 16u << 20;
	inline constexpr std::size_t kMaxRegistryFiles = 4096;
	inline constexpr std::size_t kMaxRegistryEntriesScanned = 65536;

	struct RegistryFileDiscovery
	{
		std::vector<std::filesystem::path> files;
		std::vector<std::string> problems;
		bool fatal = false;
	};

	inline std::string RegistryPathLabel(
		const std::filesystem::path& a_root, const std::filesystem::path& a_path)
	{
		try {
			const auto relative = a_path.lexically_relative(a_root);
			if (!relative.empty()) {
				return relative.generic_string();
			}
			const auto filename = a_path.filename().generic_string();
			return filename.empty() ? std::string{ "registry entry" } : filename;
		} catch (...) {
			return "registry entry";
		}
	}

	// Bounded, exception-contained discovery for author-controlled registry trees.
	// A fatal traversal/count failure returns no files, avoiding a nondeterministic
	// partial registry. Individual unreadable/oversized files are reported and skipped.
	inline RegistryFileDiscovery DiscoverRegistryFiles(
		const std::filesystem::path& a_root, std::string_view a_lowerSuffix,
		std::uintmax_t a_maxFileBytes = kMaxRegistryFileBytes,
		std::size_t a_maxFiles = kMaxRegistryFiles,
		std::size_t a_maxEntriesScanned = kMaxRegistryEntriesScanned)
	{
		namespace fs = std::filesystem;
		RegistryFileDiscovery result;

		std::error_code ec;
		if (!fs::is_directory(a_root, ec)) {
			if (ec) {
				result.problems.push_back("cannot inspect registry root: " + ec.message());
			}
			return result;
		}

		try {
			fs::recursive_directory_iterator it{
				a_root, fs::directory_options::skip_permission_denied, ec
			};
			const fs::recursive_directory_iterator end;
			if (ec) {
				result.problems.push_back("cannot open registry root: " + ec.message());
				result.fatal = true;
				return result;
			}

			std::size_t scanned = 0;
			for (; it != end;) {
				if (++scanned > a_maxEntriesScanned) {
					result.problems.push_back(
						"registry tree exceeds the " + std::to_string(a_maxEntriesScanned) +
						"-entry scan limit");
					result.fatal = true;
					break;
				}

				const fs::path path = it->path();
				std::error_code typeEc;
				const bool regular = it->is_regular_file(typeEc);
				if (typeEc) {
					result.problems.push_back("cannot inspect '" + RegistryPathLabel(a_root, path) +
						"': " + typeEc.message());
				} else if (regular && ToLower(path.filename().string()).ends_with(a_lowerSuffix)) {
					std::error_code sizeEc;
					const auto bytes = fs::file_size(path, sizeEc);
					if (sizeEc) {
						result.problems.push_back("cannot size '" + RegistryPathLabel(a_root, path) +
							"': " + sizeEc.message());
					} else if (bytes > a_maxFileBytes) {
						result.problems.push_back(
							"'" + RegistryPathLabel(a_root, path) + "' is " + std::to_string(bytes) +
							" bytes; maximum registry file size is " + std::to_string(a_maxFileBytes));
					} else if (result.files.size() >= a_maxFiles) {
						result.problems.push_back(
							"registry contains more than " + std::to_string(a_maxFiles) +
							" matching files");
						result.fatal = true;
						break;
					} else {
						result.files.push_back(path);
					}
				}

				it.increment(ec);
				if (ec) {
					result.problems.push_back("registry traversal failed below '" +
						RegistryPathLabel(a_root, path) + "': " + ec.message());
					result.fatal = true;
					break;
				}
			}
		} catch (const fs::filesystem_error& e) {
			result.problems.push_back(std::string("registry traversal failed: ") + e.code().message());
			result.fatal = true;
		} catch (const std::exception& e) {
			result.problems.push_back(std::string("registry discovery failed: ") + e.what());
			result.fatal = true;
		} catch (...) {
			result.problems.push_back("registry discovery failed with an unknown exception");
			result.fatal = true;
		}

		if (result.fatal) {
			result.files.clear();
			return result;
		}

		try {
			struct KeyedPath
			{
				std::filesystem::path path;
				std::string key;
				std::string tieBreak;
			};
			std::vector<KeyedPath> keyed;
			keyed.reserve(result.files.size());
			for (auto& file : result.files) {
				const auto display = RegistryPathLabel(a_root, file);
				keyed.push_back({ std::move(file), ToLower(display), display });
			}
			std::sort(keyed.begin(), keyed.end(), [](const KeyedPath& a_lhs, const KeyedPath& a_rhs) {
				return a_lhs.key != a_rhs.key ? a_lhs.key < a_rhs.key : a_lhs.tieBreak < a_rhs.tieBreak;
			});
			result.files.clear();
			result.files.reserve(keyed.size());
			for (auto& file : keyed) {
				result.files.push_back(std::move(file.path));
			}
		} catch (const std::exception& e) {
			result.problems.push_back(std::string("registry ordering failed: ") + e.what());
			result.files.clear();
			result.fatal = true;
		} catch (...) {
			result.problems.push_back("registry ordering failed with an unknown exception");
			result.files.clear();
			result.fatal = true;
		}
		return result;
	}
}