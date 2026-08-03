#pragma once

#include "Util/StringUtil.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
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

	enum class RegistryJsonProblemKind : std::uint8_t
	{
		kGameDirectory,
		kDiscovery,
		kFile
	};

	struct RegistryJsonSource
	{
		std::filesystem::path root;
		std::filesystem::path file;
		std::uint64_t         bytes = 0;
		std::chrono::steady_clock::time_point begun;
	};

	struct RegistryJsonVisit
	{
		std::filesystem::path root;
		std::size_t           discovered = 0;
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

	// Shared, bounded registry-load prologue: resolve Data/OSF, discover matching files in stable
	// order, open and parse comment-tolerant JSON, and contain per-file failures. Registry-specific
	// validation and reporting stay in the callbacks. a_onProblem receives nullptr for game-directory
	// and discovery failures, or the owning source for a file read/parse/callback failure.
	template <class ParseJson, class OnJson, class OnProblem>
	RegistryJsonVisit ForEachRegistryJson(std::string_view a_lowerSuffix,
		ParseJson&& a_parseJson, OnJson&& a_onJson, OnProblem&& a_onProblem,
		std::uintmax_t a_maxFileBytes = kMaxRegistryFileBytes,
		std::size_t a_maxFiles = kMaxRegistryFiles,
		std::size_t a_maxEntriesScanned = kMaxRegistryEntriesScanned)
	{
		namespace fs = std::filesystem;
		RegistryJsonVisit visit;
		std::error_code cwdError;
		const fs::path cwd = fs::current_path(cwdError);
		if (cwdError) {
			a_onProblem(RegistryJsonProblemKind::kGameDirectory, nullptr,
				"cannot resolve the game directory: " + cwdError.message());
			return visit;
		}

		visit.root = cwd / "Data" / "OSF";
		auto discovery = DiscoverRegistryFiles(
			visit.root, a_lowerSuffix, a_maxFileBytes, a_maxFiles, a_maxEntriesScanned);
		visit.discovered = discovery.files.size();
		for (const auto& problem : discovery.problems) {
			a_onProblem(RegistryJsonProblemKind::kDiscovery, nullptr, problem);
		}

		for (const auto& file : discovery.files) {
			RegistryJsonSource source;
			source.root = visit.root;
			source.file = file;
			std::error_code sizeError;
			const auto bytes = fs::file_size(file, sizeError);
			source.bytes = sizeError ? 0 : static_cast<std::uint64_t>(bytes);
			source.begun = std::chrono::steady_clock::now();
			try {
				std::ifstream input(file, std::ios::binary);
				if (!input) {
					throw std::runtime_error("file could not be opened");
				}
				a_onJson(source, a_parseJson(input));
			} catch (const std::exception& e) {
				a_onProblem(RegistryJsonProblemKind::kFile, &source, e.what());
			} catch (...) {
				a_onProblem(RegistryJsonProblemKind::kFile, &source, "unknown exception");
			}
		}
		return visit;
	}
}
