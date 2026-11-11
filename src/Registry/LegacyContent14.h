#pragma once
#include "Util/StringUtil.h"
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace OSF::Registry::Legacy14
{
	using Json = nlohmann::json;
	struct Identity
	{
		std::string                                     scene, local;
		std::shared_ptr<const std::vector<std::string>> stages;
		std::string                                     use;
		bool                                            single = false, synthetic = false;
	};
	struct Conversion
	{
		Json                            document;
		std::map<std::string, Identity> identities;
	};
	inline std::string Key(std::string value) { return Util::ToLower(value); }
	inline std::string Address(const std::string& scene, const std::string& node)
	{
		static constexpr char hex[] = "0123456789abcdef";
		std::string           out = "osf.compat14/";
		for (const auto& part : { Key(scene), Key(node) }) {
			for (unsigned char c : part) {
				out += hex[c >> 4];
				out += hex[c & 15];
			}
			out += '/';
		}
		return out;
	}
	inline void Aliases(Json& value)
	{
		if (!value.is_object())
			return;
		for (const auto& [oldName, newName] : { std::pair{ "stripActors", "hideApparel" }, { "lockPlayer", "playerInputLock" }, { "playerControl", "sceneControls" } }) {
			if (value.contains(oldName)) {
				if (value.contains(newName))
					throw std::runtime_error("conflicting legacy and canonical policy fields");
				value[newName] = value.at(oldName);
				value.erase(oldName);
			}
		}
		if (value.contains("inPlace")) {
			if (value.contains("placement") || !value.at("inPlace").is_boolean())
				throw std::runtime_error("invalid legacy placement");
			value["placement"] = value.at("inPlace").get<bool>() ? "followActor" : "anchorAndPin";
			value.erase("inPlace");
		}
		// Only policy-bearing objects; custom action payloads are not rewritten.
		for (const char* key : { "roles", "scenes", "nodes" }) {
			if (auto it = value.find(key); it != value.end() && (it->is_array() || it->is_object()))
				for (auto& child : *it) Aliases(child);
		}
	}
	inline Conversion Convert(Json input)
	{
		Conversion out;
		Aliases(input);
		out.document = input;
		out.document["schema"] = 2;
		out.document.erase("scenes");
		out.document.erase("id");
		out.document.erase("nodes");
		out.document.erase("entry");
		out.document.erase("clip");
		out.document.erase("stages");
		out.document.erase("linearStages");
		out.document["nodes"] = Json::array();
		Json scenes = input.contains("scenes") ? input.at("scenes") : Json::array();
		if (input.contains("id"))
			scenes.push_back(input);
		if (!scenes.is_array())
			throw std::runtime_error("legacy scenes must be an array");
		if (scenes.size() > 4096)
			throw std::runtime_error("legacy scene limit exceeded");
		for (const auto& scene : scenes) {
			const auto id = scene.at("id").get<std::string>();
			if (id.empty())
				throw std::runtime_error("empty legacy scene id");
			const bool graph = scene.contains("nodes");
			auto       nodes = graph ? scene.at("nodes") : Json::array();
			if (!nodes.is_array())
				throw std::runtime_error("legacy nodes must be an array");
			std::vector<std::string> locals;
			if (!graph) {
				auto stages = scene.contains("stages") ? scene.at("stages") : Json::array({ Json{ { "clips", Json::array({ scene.at("clip") }) } } });
				if (!stages.is_array() || stages.empty() || stages.size() > 4096)
					throw std::runtime_error("invalid legacy stages");
				for (std::size_t i = 0; i < stages.size(); ++i) {
					const auto local = "#s" + std::to_string(i);
					auto       stage = stages.at(i);
					if (stage.is_array())
						stage = Json{ { "clips", stage } };
					Json node = { { "id", local } };
					for (const char* lane : { "cue", "action", "sound", "camera" }) {
						if (stage.contains(lane)) {
							node[lane] = stage.at(lane);
							stage.erase(lane);
						}
					}
					const float timer = stage.value("timer", 0.0f);
					const int   loops = stage.value("loops", stage.contains("timer") ? 0 : 1);
					node["stages"] = Json::array({ stage });
					node["loops"] = loops;
					if (timer > 0)
						node["timer"] = timer;
					const auto target = i + 1 == stages.size() ? "$end" : "#s" + std::to_string(i + 1);
					node["edges"] = Json::array({ Json{ { "when", "advance" }, { "default", true }, { "to", target }, { "id", "compat.advance" }, { "label", "Next" } } });
					if (loops > 0)
						node["edges"].push_back({ { "when", "loops" }, { "to", target } });
					if (timer > 0)
						node["edges"].push_back({ { "when", "timer" }, { "to", target } });
					nodes.push_back(node);
					locals.push_back(local);
				}
			} else if (scene.contains("linearStages"))
				locals = scene.at("linearStages").get<std::vector<std::string>>();
			if (nodes.empty() || nodes.size() > 4096)
				throw std::runtime_error("invalid legacy nodes");
			const auto                         entry = graph ? scene.value("entry", nodes.front().at("id").get<std::string>()) : "#s0";
			std::map<std::string, std::string> addresses;
			for (const auto& node : nodes) {
				const auto local = node.at("id").get<std::string>();
				if (local.empty() || !addresses.emplace(Key(local), Key(local) == Key(entry) ? id : Address(id, local)).second)
					throw std::runtime_error("duplicate/empty legacy node id");
			}
			if (!addresses.contains(Key(entry)))
				throw std::runtime_error("missing legacy entry");
			std::vector<std::string> mapping;
			for (const auto& local : locals) mapping.push_back(addresses.at(Key(local)));
			const auto stageMap = std::make_shared<const std::vector<std::string>>(std::move(mapping));
			// The old container owns a single cast across every local node.
			Json roles;
			if (scene.contains("roles"))
				roles = scene.at("roles");
			else if (input.contains("roles") && input.at("roles").is_array())
				roles = input.at("roles");
			else {
				for (const auto& node : nodes)
					if (Key(node.at("id").get<std::string>()) == Key(entry)) {
						if (!node.contains("stages"))
							throw std::runtime_error("legacy use entry requires explicit roles");
						const auto& first = node.at("stages").at(0);
						const auto  count = first.is_array() ? first.size() : first.at("clips").size();
						roles = Json::array();
						for (std::size_t i = 0; i < count; ++i) roles.push_back(Json::object());
					}
			}
			if (!roles.is_array() || roles.empty())
				throw std::runtime_error("invalid legacy cast");
			for (auto node : nodes) {
				std::string use;
				if (node.contains("use")) {
					use = node.at("use").get<std::string>();
					if (use.empty() || node.contains("stages"))
						throw std::runtime_error("legacy use requires a scene ID and no inline stages");
					node.erase("use");
					// Private placeholder, replaced from the validated target before probing or publication.
					Json clips = Json::array();
					for (std::size_t i = 0; i < roles.size(); ++i) clips.push_back("__compat_pending__.af");
					node["stages"] = Json::array({ Json{ { "clips", clips } } });
				}
				if (graph) {
					bool timerEdge = false;
					if (const auto edges = node.find("edges"); edges != node.end())
						for (const auto& edge : *edges)
							if (Key(edge.value("when", std::string{})) == "timer")
								timerEdge = true;
					// Published graph policy discarded terminal stage timers without a timer route.
					if (!timerEdge)
						node["timer"] = 0;
				}
				if (graph && !node.contains("loops")) {
					auto& terminal = node["stages"].back();
					if (terminal.is_array())
						terminal = Json{ { "clips", terminal } };
					terminal["loops"] = 1;  // 1.4 omitted node loops means once, regardless of terminal stage budget.
				}
				const auto local = node.at("id").get<std::string>();
				const auto global = addresses.at(Key(local));
				Json       merged = scene;
				for (const char* field : { "nodes", "scenes", "entry", "linearStages", "clip", "stages", "schema" }) merged.erase(field);
				merged.update(node);
				merged["id"] = global;
				merged["roles"] = roles;
				// A file camera default applied only on the container entry in 1.4.
				if (Key(local) != Key(entry) && !node.contains("camera"))
					merged["camera"] = "none";
				if (Key(local) != Key(entry))
					merged["unlisted"] = true;
				for (auto& edge : merged["edges"]) {
					const auto target = edge.at("to").get<std::string>();
					if (target != "$end")
						edge["to"] = addresses.at(Key(target));
				}
				if (merged["edges"].is_null())
					merged.erase("edges");
				if (!out.identities.emplace(Key(global), Identity{ id, local, stageMap, use, nodes.size() == 1, !graph }).second)
					throw std::runtime_error("legacy generated ID collision");
				out.document["nodes"].push_back(std::move(merged));
				if (out.identities.size() > 8192)
					throw std::runtime_error("legacy converted node limit exceeded");
			}
		}
		return out;
	}
}
