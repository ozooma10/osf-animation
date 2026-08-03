#include "Registry/RequirementRegistry.h"

#include "API/MinimumVersion.h"
#include "Util/RegistryFiles.h"
#include "Util/StringUtil.h"

#include <algorithm>
#include <nlohmann/json.hpp>

#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string_view>

namespace OSF::Registry::RequirementRegistry
{
	namespace
	{
		struct Version
		{
			std::uint32_t major{};
			std::uint32_t minor{};
			std::uint32_t patch{};

			auto operator<=>(const Version&) const = default;
		};

		struct Requirement
		{
			std::string id;
			std::string name;
			Version minimum;
			std::string source;
		};

		constexpr std::uintmax_t kMaxManifestBytes = 64u << 10;

		Version ParseVersion(std::string_view a_text)
		{
			Version version;
			std::uint32_t* parts[]{ &version.major, &version.minor, &version.patch };
			for (std::size_t i = 0; i < std::size(parts); ++i) {
				const auto dot = a_text.find('.');
				const auto token = dot == std::string_view::npos ? a_text : a_text.substr(0, dot);
				if (token.empty()) {
					throw std::runtime_error("requires.osf.animation must be an exact major.minor.patch version");
				}
				const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), *parts[i]);
				if (error != std::errc{} || end != token.data() + token.size()) {
					throw std::runtime_error("requires.osf.animation must be an exact major.minor.patch version");
				}
				if (i < 2) {
					if (dot == std::string_view::npos) {
						throw std::runtime_error("requires.osf.animation must be an exact major.minor.patch version");
					}
					a_text.remove_prefix(dot + 1);
				} else if (dot != std::string_view::npos) {
					throw std::runtime_error("requires.osf.animation must be an exact major.minor.patch version");
				}
			}
			return version;
		}

		std::string ReadString(const nlohmann::json& a_object, const char* a_key)
		{
			const auto it = a_object.find(a_key);
			if (it == a_object.end() || !it->is_string()) {
				throw std::runtime_error(std::string(a_key) + " must be a string");
			}
			return it->get<std::string>();
		}

		bool ValidId(std::string_view a_id)
		{
			return !a_id.empty() && a_id.size() <= 96 &&
				std::all_of(a_id.begin(), a_id.end(), [](unsigned char a_ch) {
					return (a_ch >= 'a' && a_ch <= 'z') || (a_ch >= 'A' && a_ch <= 'Z') ||
						(a_ch >= '0' && a_ch <= '9') || a_ch == '.' || a_ch == '_' || a_ch == '-';
				});
		}

		Requirement Parse(const nlohmann::json& a_json, std::string a_source)
		{
			if (!a_json.is_object()) {
				throw std::runtime_error("root must be an object");
			}
			const auto schema = a_json.find("schema");
			if (schema == a_json.end() || !schema->is_number_integer() || schema->get<std::int64_t>() != 1) {
				throw std::runtime_error("schema must be 1");
			}

			const auto consumer = a_json.find("consumer");
			if (consumer == a_json.end() || !consumer->is_object()) {
				throw std::runtime_error("consumer must be an object");
			}
			const auto dependencies = a_json.find("requires");
			if (dependencies == a_json.end() || !dependencies->is_object()) {
				throw std::runtime_error("requires must be an object");
			}

			Requirement result;
			result.id = ReadString(*consumer, "id");
			result.name = ReadString(*consumer, "name");
			if (!ValidId(result.id)) {
				throw std::runtime_error("consumer.id must use 1-96 letters, numbers, '.', '_', or '-'");
			}
			result.minimum = ParseVersion(ReadString(*dependencies, "osf.animation"));
			result.source = std::move(a_source);
			return result;
		}
	}

	void LoadAll()
	{
		std::map<std::string, Requirement> requirements;
		std::size_t problems = 0;

		const auto visit = Util::ForEachRegistryJson(".requirements.json",
			[](std::ifstream& a_input) {
				return nlohmann::json::parse(a_input, nullptr, true, true);
			},
			[&](const Util::RegistryJsonSource& a_source, const nlohmann::json& a_json) {
				const std::string source = Util::RegistryPathLabel(a_source.root, a_source.file);
				auto requirement = Parse(a_json, source);
				const std::string key = Util::ToLower(requirement.id);
				auto [found, inserted] = requirements.try_emplace(key, requirement);
				if (!inserted) {
					++problems;
					REX::WARN("[Registry] duplicate requirement consumer id '{}' in '{}' and '{}'; highest minimum wins",
						requirement.id, found->second.source, source);
					if (found->second.minimum < requirement.minimum) {
						found->second = std::move(requirement);
					}
				}
			},
			[&](Util::RegistryJsonProblemKind a_kind, const Util::RegistryJsonSource* a_source,
				const std::string& a_message) {
				++problems;
				if (a_kind == Util::RegistryJsonProblemKind::kGameDirectory) {
					REX::ERROR("[Registry] requirement discovery {}", a_message);
				} else if (a_kind == Util::RegistryJsonProblemKind::kDiscovery) {
					REX::ERROR("[Registry] requirement discovery: {}", a_message);
				} else if (a_source) {
					const std::string source = Util::RegistryPathLabel(a_source->root, a_source->file);
					if (a_message == "unknown exception") {
						REX::ERROR("[Registry] skipping requirement manifest '{}' after an unknown error", source);
					} else {
						REX::ERROR("[Registry] skipping requirement manifest '{}': {}", source, a_message);
					}
				}
			}, kMaxManifestBytes);

		std::size_t accepted = 0;
		for (const auto& [_, requirement] : requirements) {
			const auto result = API::MinimumVersion::ReportForConsumer(
				requirement.id.c_str(), requirement.name.c_str(),
				requirement.minimum.major, requirement.minimum.minor, requirement.minimum.patch);
			if (result != API::MinimumVersionResult::kInvalidRequest) {
				++accepted;
			} else {
				++problems;
				REX::ERROR("[Registry] invalid consumer id or name in requirement manifest '{}'", requirement.source);
			}
		}

		REX::INFO("[Registry] loaded {} requirement manifest(s): {} consumer(s), {} problem(s)",
			visit.discovered, accepted, problems);
	}
}
