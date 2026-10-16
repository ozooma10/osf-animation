#pragma once

#include "Animation/Scene.h"
#include "Util/Math.h"
#include <map>
#include <nlohmann/json.hpp>

namespace OSF::Scene::Alignment
{
	using Delta = Animation::ParticipantPlacement;
	using Roles = std::map<std::string, Delta, std::less<>>;
	using Nodes = std::map<std::string, Roles, std::less<>>;
	using Table = std::map<std::string, Nodes, std::less<>>;

	inline bool Valid(const Delta& d) { return std::isfinite(d.x) && std::isfinite(d.y) && std::isfinite(d.z) && std::isfinite(d.heading); }
	inline bool Equal(const Delta& a, const Delta& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.heading == b.heading; }
	inline Delta Find(const Table& table, std::string_view scene, std::string_view node, std::string_view role)
	{
		const auto s = table.find(scene); if (s == table.end()) return {};
		const auto n = s->second.find(node); if (n == s->second.end()) return {};
		const auto r = n->second.find(role); return r == n->second.end() ? Delta{} : r->second;
	}
	inline void Put(Table& table, const std::string& scene, const std::string& node, const std::string& role, const Delta& delta)
	{
		if (!Equal(delta, {})) { table[scene][node][role] = delta; return; }
		const auto s = table.find(scene); if (s == table.end()) return;
		const auto n = s->second.find(node); if (n == s->second.end()) return;
		n->second.erase(role);
		if (n->second.empty()) s->second.erase(n);
		if (s->second.empty()) table.erase(s);
	}
	inline std::optional<Table> Parse(const nlohmann::json& document)
	{
		try {
			if (!document.is_object() || document.at("formatVersion") != 1 || !document.at("scenes").is_object()) return std::nullopt;
			Table table;
			std::size_t rows = 0;
			const auto validKey = [](const std::string& key) { return !key.empty() && key.size() <= 4096 && key.find('\0') == std::string::npos; };
			for (const auto& [scene, nodes] : document.at("scenes").items()) {
				if (!validKey(scene) || !nodes.is_object()) return std::nullopt;
				for (const auto& [node, roles] : nodes.items()) {
					if (!validKey(node) || !roles.is_object()) return std::nullopt;
					for (const auto& [role, value] : roles.items()) {
						if (!validKey(role) || !value.is_object() || ++rows > 8192) return std::nullopt;
						Delta d;
						const auto component = [&](const char* key) { const auto it = value.find(key); if (it == value.end()) return 0.0f; if (!it->is_number()) throw std::runtime_error("non-numeric alignment"); return it->get<float>(); };
						d.x = component("x"); d.y = component("y"); d.z = component("z"); d.heading = component("heading") * Util::kDegToRadF;
						if (!Valid(d)) return std::nullopt;
						Put(table, scene, node, role, d);
					}
				}
			}
			return table;
		} catch (...) { return std::nullopt; }
	}
	inline nlohmann::json Serialize(const Table& table)
	{
		nlohmann::json scenes = nlohmann::json::object();
		for (const auto& [scene, nodes] : table) for (const auto& [node, roles] : nodes) for (const auto& [role, d] : roles) {
			if (!Equal(d, {})) scenes[scene][node][role] = { { "x", d.x }, { "y", d.y }, { "z", d.z }, { "heading", d.heading / Util::kDegToRadF } };
		}
		return { { "formatVersion", 1 }, { "scenes", std::move(scenes) } };
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
