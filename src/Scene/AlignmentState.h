#pragma once

#include "Animation/Scene.h"
#include "Util/Math.h"
#include "Util/StringUtil.h"
#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>

namespace OSF::Scene::Alignment
{
	using Delta = Animation::ParticipantPlacement;
	using Roles = std::map<std::string, Delta, std::less<>>;
	using Table = std::map<std::string, Roles, std::less<>>;

	inline auto FindRole(const std::vector<std::string>& roles, std::string_view role)
	{
		const auto key = Util::ToLower(role);
		return std::ranges::find_if(roles, [&](const auto& name) { return Util::ToLower(name) == key; });
	}
	inline std::vector<std::string> RoleKeys(const std::vector<std::string>& names)
	{
		auto keys = names;
		for (std::size_t i = 0; i < keys.size(); ++i) {
			if (!keys[i].empty()) continue;
			auto key = "#" + std::to_string(i);
			// Authored names keep their meaning, even if one looks like an index key.
			while (FindRole(names, key) != names.end()) key.insert(key.begin(), '#');
			keys[i] = std::move(key);
		}
		return keys;
	}

	inline bool Valid(const Delta& d) { return std::isfinite(d.x) && std::isfinite(d.y) && std::isfinite(d.z) && std::isfinite(d.heading); }
	inline bool Equal(const Delta& a, const Delta& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.heading == b.heading; }
	inline Delta Find(const Table& table, std::string_view node, std::string_view role)
	{
		const auto n = table.find(Util::ToLower(node)); if (n == table.end()) return {};
		const auto r = n->second.find(Util::ToLower(role)); return r == n->second.end() ? Delta{} : r->second;
	}
	inline void Put(Table& table, const std::string& node, const std::string& role, const Delta& delta)
	{
		const auto nodeKey = Util::ToLower(node);
		const auto roleKey = Util::ToLower(role);
		if (!Equal(delta, {})) { table[nodeKey][roleKey] = delta; return; }
		const auto n = table.find(nodeKey); if (n == table.end()) return;
		n->second.erase(roleKey);
		if (n->second.empty()) table.erase(n);
	}
	inline std::optional<Table> Parse(const nlohmann::json& document, std::size_t* skipped = nullptr)
	{
		if (skipped) *skipped = 0;
		const auto skip = [&] { if (skipped) ++*skipped; };
		try {
			if (!document.is_object() || document.at("formatVersion") != 2 || !document.at("nodes").is_object()) return std::nullopt;
			Table table;
			std::size_t rows = 0;
			const auto validKey = [](const std::string& key) { return !key.empty() && key.size() <= 4096 && key.find('\0') == std::string::npos; };
			for (const auto& [node, roles] : document.at("nodes").items()) {
				if (!validKey(node) || !roles.is_object()) { skip(); continue; }
				for (const auto& [role, value] : roles.items()) {
					if (++rows > 8192) return std::nullopt;
					if (!validKey(role) || !value.is_object()) { skip(); continue; }
					try {
						Delta d;
						const auto component = [&](const char* key) { const auto it = value.find(key); if (it == value.end()) return 0.0f; if (!it->is_number()) throw std::runtime_error("non-numeric alignment"); return it->get<float>(); };
						d.x = component("x"); d.y = component("y"); d.z = component("z"); d.heading = component("heading") * Util::kDegToRadF;
						if (!Valid(d)) { skip(); continue; }
						Put(table, node, role, d);
					} catch (...) { skip(); }
				}
			}
			return table;
		} catch (...) { return std::nullopt; }
	}
	inline nlohmann::json Serialize(const Table& table)
	{
		nlohmann::json nodes = nlohmann::json::object();
		for (const auto& [node, roles] : table) for (const auto& [role, d] : roles) {
			if (!Equal(d, {})) nodes[node][role] = { { "x", d.x }, { "y", d.y }, { "z", d.z }, { "heading", d.heading / Util::kDegToRadF } };
		}
		return { { "formatVersion", 2 }, { "nodes", std::move(nodes) } };
	}
	struct WorkingCopy
	{
		std::map<std::string, std::vector<Delta>, std::less<>> nodes;
		std::vector<Delta>& Enter(const std::string& node, std::vector<Delta> seed)
		{
			return nodes.try_emplace(node, std::move(seed)).first->second;
		}
	};
}
