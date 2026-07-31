#include "API/MinimumVersion.h"

#include "API/Health.h"
#include "UI/HudMessage.h"
#include "Util/StringUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <compare>
#include <format>
#include <map>
#include <mutex>
#include <vector>

namespace OSF::API::MinimumVersion
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
			std::string consumer;
			Version     required;
			Version     notified;
		};

		constexpr std::size_t kMaxConsumerLength = 96;

		std::mutex                         g_lock;
		std::map<std::string, Requirement> g_requirements;
		std::map<std::string, std::string> g_aliases;
		bool                               g_promptsEnabled = false;

		Version InstalledVersion()
		{
			const auto v = SFSE::GetPluginVersion();
			return { v.major(), v.minor(), v.patch() };
		}

		std::string FormatVersion(const Version& a_version)
		{
			return std::format("{}.{}.{}", a_version.major, a_version.minor, a_version.patch);
		}

		std::string ConsumerLabel(const char* a_consumer)
		{
			if (!a_consumer) {
				return {};
			}
			const std::size_t length = ::strnlen(a_consumer, kMaxConsumerLength + 1);
			if (length == 0 || length > kMaxConsumerLength) {
				return {};
			}
			std::string label(a_consumer, length);
			const auto visible = [](unsigned char a_ch) { return !std::isspace(a_ch); };
			const auto first = std::find_if(label.begin(), label.end(), visible);
			const auto last = std::find_if(label.rbegin(), label.rend(), visible).base();
			if (first >= last) {
				return {};
			}
			label = std::string(first, last);
			if (std::any_of(label.begin(), label.end(), [](unsigned char a_ch) {
					return std::iscntrl(a_ch) || a_ch == '\\' || a_ch == '/';
				})) {
				return {};
			}
			return label;
		}

		std::string ConsumerId(const char* a_id)
		{
			const auto id = ConsumerLabel(a_id);
			return id.empty() ? std::string{} : Util::ToLower(id);
		}

		void Publish(const Requirement& a_requirement, const Version& a_installed)
		{
			const std::string required = FormatVersion(a_requirement.required);
			const std::string installed = FormatVersion(a_installed);
			const nlohmann::json context{
				{ "consumerId", a_requirement.id },
				{ "consumer", a_requirement.consumer },
				{ "installedVersion", installed },
				{ "minimumVersion", required },
			};
			Health::Report("consumer-version:" + a_requirement.id,
				"compat.needs-newer-osf-animation", Health::Severity::kWarning,
				a_requirement.consumer, &context);
			REX::WARN("[API] '{}' requires OSF Animation v{} or newer; installed version is v{}",
				a_requirement.consumer, required, installed);
		}

		void ShowOne(const Requirement& a_requirement)
		{
			UI::HudMessage::Warning(std::format(
				"{} requires OSF Animation v{} or newer. Upgrade OSF Animation.",
				a_requirement.consumer, FormatVersion(a_requirement.required)));
		}
	}

	MinimumVersionResult Report(
		const char* a_consumer, std::uint32_t a_major,
		std::uint32_t a_minor, std::uint32_t a_patch)
	{
		return ReportForConsumer(a_consumer, a_consumer, a_major, a_minor, a_patch);
	}

	MinimumVersionResult ReportForConsumer(
		const char* a_consumerId, const char* a_consumerName,
		std::uint32_t a_major, std::uint32_t a_minor, std::uint32_t a_patch)
	{
		const std::string id = ConsumerId(a_consumerId);
		const std::string consumer = ConsumerLabel(a_consumerName);
		const std::string nameKey = Util::ToLower(consumer);
		if (id.empty() || consumer.empty()) {
			REX::WARN("[API] rejected minimum-version report with an invalid consumer id or name");
			return MinimumVersionResult::kInvalidRequest;
		}

		const Version installed = InstalledVersion();
		const Version required{ a_major, a_minor, a_patch };
		if (installed >= required) {
			return MinimumVersionResult::kSupported;
		}

		Requirement published;
		bool        publish = false;
		bool        prompt = false;
		{
			std::lock_guard lock(g_lock);
			std::string key = id;
			if (id != nameKey) {
				g_aliases[nameKey] = id;
				if (auto legacy = g_requirements.find(nameKey); legacy != g_requirements.end()) {
					const std::string legacyHealthId = "consumer-version:" + nameKey;
					auto [target, inserted] = g_requirements.try_emplace(
						id, Requirement{ id, consumer, legacy->second.required, legacy->second.notified });
					if (!inserted) {
						if (target->second.required < legacy->second.required) {
							target->second.required = legacy->second.required;
						}
						if (target->second.notified < legacy->second.notified) {
							target->second.notified = legacy->second.notified;
						}
					}
					target->second.id = id;
					target->second.consumer = consumer;
					g_requirements.erase(legacy);
					Health::Clear(legacyHealthId);
					publish = true;
				}
			} else if (const auto alias = g_aliases.find(id); alias != g_aliases.end()) {
				key = alias->second;
			}
			auto [it, inserted] = g_requirements.try_emplace(
				key, Requirement{ key, consumer, required, {} });
			auto& entry = it->second;
			if (inserted || entry.required < required) {
				entry.consumer = consumer;
				entry.required = required;
				publish = true;
			}
			if (g_promptsEnabled && entry.notified < entry.required) {
				entry.notified = entry.required;
				prompt = true;
			}
			if (publish) {
				Publish(entry, installed);
			}
			published = entry;
		}

		if (prompt) {
			ShowOne(published);
		}
		return MinimumVersionResult::kUpgradeRequired;
	}

	void EnablePrompts()
	{
		std::vector<Requirement> pending;
		{
			std::lock_guard lock(g_lock);
			if (g_promptsEnabled) {
				return;
			}
			g_promptsEnabled = true;
			for (auto& [_, requirement] : g_requirements) {
				if (requirement.notified < requirement.required) {
					requirement.notified = requirement.required;
					pending.push_back(requirement);
				}
			}
		}

		if (pending.empty()) {
			return;
		}
		if (pending.size() == 1) {
			ShowOne(pending.front());
			return;
		}

		const auto highest = std::max_element(pending.begin(), pending.end(),
			[](const Requirement& a_lhs, const Requirement& a_rhs) {
				return a_lhs.required < a_rhs.required;
			});
		UI::HudMessage::Warning(std::format(
			"{} installed mods require OSF Animation v{} or newer. Upgrade OSF Animation.",
			pending.size(), FormatVersion(highest->required)));
	}
}

extern "C" __declspec(dllexport) std::uint32_t OSF_ReportMinimumVersion(
	const char* a_consumer, std::uint32_t a_major,
	std::uint32_t a_minor, std::uint32_t a_patch) noexcept
{
	try {
		return static_cast<std::uint32_t>(
			OSF::API::MinimumVersion::Report(a_consumer, a_major, a_minor, a_patch));
	} catch (...) {
		REX::ERROR("[API] minimum-version report failed with an unexpected exception");
		return static_cast<std::uint32_t>(
			OSF::API::MinimumVersionResult::kInvalidRequest);
	}
}
