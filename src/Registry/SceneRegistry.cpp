#include "Registry/SceneRegistry.h"

#include "Util/StringUtil.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace OSF::Registry
{
	using OSF::Util::ToLower;

	namespace
	{
		using json = nlohmann::json;

		SlotGender ParseRoleGender(const json& a_role)
		{
			const auto s = ToLower(a_role.value("gender", "any"));
			if (s == "male" || s == "m") {
				return SlotGender::kMale;
			}
			if (s == "female" || s == "f") {
				return SlotGender::kFemale;
			}
			return SlotGender::kAny;
		}

		LoopMode ParseLoop(const json& a_node, std::int32_t& a_count)
		{
			a_count = 0;
			const auto it = a_node.find("loop");
			if (it == a_node.end() || !it->is_object()) {
				throw std::runtime_error("missing object 'loop' { mode, ... }");
			}
			const auto mode = ToLower(it->value("mode", std::string{}));
			if (mode == "once") {
				return LoopMode::kOnce;
			}
			if (mode == "hold") {
				return LoopMode::kHold;
			}
			if (mode == "count") {
				a_count = it->value("count", 0);
				if (a_count <= 0) {
					throw std::runtime_error("loop mode 'count' requires integer count > 0");
				}
				return LoopMode::kCount;
			}
			throw std::runtime_error("unknown loop.mode '" + mode + "'");
		}

		EdgeWhen ParseWhen(const std::string& a_when, std::string& a_trigger)
		{
			if (a_when == "end") {
				return EdgeWhen::kEnd;
			}
			if (a_when == "loops") {
				return EdgeWhen::kLoops;
			}
			if (a_when == "timer") {
				return EdgeWhen::kTimer;
			}
			if (a_when == "advance") {
				return EdgeWhen::kAdvance;
			}
			if (a_when.rfind("trigger:", 0) == 0) {
				a_trigger = a_when.substr(8);
				return EdgeWhen::kTrigger;
			}
			throw std::runtime_error("unknown edge 'when' value '" + a_when + "' (cond:<expr> is deferred)");
		}

		SceneEdge ParseEdge(const json& a_edge, const std::string& a_nodeId)
		{
			SceneEdge e;
			e.to = a_edge.value("to", std::string{});
			if (e.to.empty()) {
				throw std::runtime_error("node '" + a_nodeId + "': an edge is missing 'to'");
			}
			e.when = ParseWhen(ToLower(a_edge.value("when", "advance")), e.trigger);
			e.id = a_edge.value("id", std::string{});
			e.label = a_edge.value("label", std::string{});
			e.labelKey = a_edge.value("labelKey", std::string{});
			e.isDefault = a_edge.value("default", false);
			e.priority = a_edge.value("priority", 0);
			// branchable (advance) edges feed GetSceneEdge* menus -> need id + label.
			if (e.when == EdgeWhen::kAdvance && (e.id.empty() || e.label.empty())) {
				throw std::runtime_error("node '" + a_nodeId + "': a branchable (advance) edge requires 'id' and 'label'");
			}
			return e;
		}

		SceneNode ParseNode(const json& a_node, std::vector<std::string>& a_warnings, const std::string& a_sceneId)
		{
			SceneNode n;
			n.id = a_node.value("id", std::string{});
			if (n.id.empty()) {
				throw std::runtime_error("a node is missing 'id'");
			}
			n.anim = a_node.value("anim", std::string{});
			if (n.anim.empty()) {
				throw std::runtime_error("node '" + n.id + "': missing 'anim'");
			}
			n.loopMode = ParseLoop(a_node, n.loopCount);
			n.timerSec = a_node.value("timerSec", 0.0f);
			n.loopForever = a_node.value("loopForever", false);
			if (const auto it = a_node.find("slots"); it != a_node.end()) {
				for (const auto& s : *it) {
					n.slots.push_back(s.get<std::string>());
				}
			}

			std::unordered_set<std::string> edgeIds;
			int defaults = 0;
			bool hasTimerEdge = false;
			if (const auto it = a_node.find("edges"); it != a_node.end()) {
				for (const auto& jEdge : *it) {
					auto e = ParseEdge(jEdge, n.id);
					if (e.when == EdgeWhen::kEnd && n.loopMode == LoopMode::kHold) {
						throw std::runtime_error("node '" + n.id + "': an 'end' edge on a hold node can never fire");
					}
					if (e.when == EdgeWhen::kLoops && n.loopMode != LoopMode::kCount) {
						throw std::runtime_error("node '" + n.id + "': a 'loops' edge needs loop mode 'count'");
					}
					if (e.when == EdgeWhen::kTimer) {
						hasTimerEdge = true;
					}
					if (e.isDefault) {
						defaults++;
					}
					if (!e.id.empty() && !edgeIds.insert(ToLower(e.id)).second) {
						throw std::runtime_error("node '" + n.id + "': duplicate edge id '" + e.id + "'");
					}
					n.edges.push_back(std::move(e));
				}
			}
			if (defaults > 1) {
				throw std::runtime_error("node '" + n.id + "': more than one default advance edge");
			}
			if (hasTimerEdge && n.timerSec <= 0.0f) {
				throw std::runtime_error("node '" + n.id + "': has a 'timer' edge but timerSec <= 0");
			}
			if (!hasTimerEdge && n.timerSec > 0.0f) {
				a_warnings.push_back("scene '" + a_sceneId + "' node '" + n.id + "': timerSec set but no 'timer' edge");
			}
			return n;
		}

		// Throws std::runtime_error on any reject; pushes non-fatal notes to a_warnings.
		SceneDef ParseScene(const json& a_json, std::vector<std::string>& a_warnings)
		{
			SceneDef def;
			def.id = a_json.value("id", std::string{});
			if (def.id.empty()) {
				throw std::runtime_error("scene missing 'id'");
			}
			def.name = a_json.value("name", def.id);
			def.priority = a_json.value("priority", 0);
			def.entry = a_json.value("entry", std::string{});
			if (def.entry.empty()) {
				throw std::runtime_error("scene '" + def.id + "': missing 'entry'");
			}
			if (const auto it = a_json.find("tags"); it != a_json.end()) {
				for (const auto& t : *it) {
					def.tags.push_back(t.get<std::string>());
				}
			}
			if (const auto it = a_json.find("roles"); it != a_json.end()) {
				for (const auto& jRole : *it) {
					SceneRole r;
					r.name = jRole.value("name", std::string{});
					if (r.name.empty()) {
						throw std::runtime_error("scene '" + def.id + "': a role is missing 'name'");
					}
					r.gender = ParseRoleGender(jRole);
					def.roles.push_back(std::move(r));
				}
			}

			const auto jNodes = a_json.find("nodes");
			if (jNodes == a_json.end() || !jNodes->is_array() || jNodes->empty()) {
				throw std::runtime_error("scene '" + def.id + "': 'nodes' must be a non-empty array");
			}
			std::unordered_set<std::string> nodeIds;
			for (const auto& jNode : *jNodes) {
				auto n = ParseNode(jNode, a_warnings, def.id);
				if (!nodeIds.insert(ToLower(n.id)).second) {
					throw std::runtime_error("scene '" + def.id + "': duplicate node id '" + n.id + "'");
				}
				def.nodes.push_back(std::move(n));
			}

			// Optional linear-stage map (stage i -> node id). Each must name a real node.
			if (const auto it = a_json.find("linearStages"); it != a_json.end()) {
				for (const auto& s : *it) {
					auto nid = s.get<std::string>();
					if (!nodeIds.count(ToLower(nid))) {
						throw std::runtime_error("scene '" + def.id + "': linearStages references missing node '" + nid + "'");
					}
					def.linearStages.push_back(std::move(nid));
				}
			}

			// Reference validation (needs the full node-id set).
			if (!nodeIds.count(ToLower(def.entry))) {
				throw std::runtime_error("scene '" + def.id + "': entry '" + def.entry + "' is not a node");
			}
			for (const auto& n : def.nodes) {
				for (const auto& e : n.edges) {
					if (e.to != "$end" && !nodeIds.count(ToLower(e.to))) {
						throw std::runtime_error("scene '" + def.id + "': node '" + n.id + "' edge targets missing node '" + e.to + "'");
					}
				}
				if (n.loopMode == LoopMode::kHold && !n.loopForever) {
					bool hasExit = false;
					for (const auto& e : n.edges) {
						if (e.when == EdgeWhen::kAdvance || e.when == EdgeWhen::kTimer || e.when == EdgeWhen::kTrigger) {
							hasExit = true;
							break;
						}
					}
					if (!hasExit) {
						a_warnings.push_back("scene '" + def.id + "' node '" + n.id +
							"': hold node with no advance/timer/trigger edge never ends (set loopForever:true if intended)");
					}
				}
			}
			return def;
		}

		void LoadSceneFile(const json& a_json, const std::filesystem::path& a_file,
			std::unordered_map<std::string, SceneDef>& a_out, std::vector<std::string>& a_errors)
		{
			const std::string fileName = a_file.filename().string();

			const auto it = a_json.find("schema");
			if (it == a_json.end()) {
				a_errors.push_back("[error] '" + fileName + "': missing required 'schema'");
				REX::ERROR("SceneRegistry: '{}' missing required 'schema' — skipped", fileName);
				return;
			}
			if (!it->is_number_integer()) {
				a_errors.push_back("[error] '" + fileName + "': non-integer 'schema'");
				REX::ERROR("SceneRegistry: '{}' has a non-integer 'schema' — skipped", fileName);
				return;
			}
			const auto schema = it->get<std::int64_t>();
			if (schema < 1 || schema > kSceneSchemaVersion) {
				a_errors.push_back("[error] '" + fileName + "': scene schema " + std::to_string(schema) +
					" unsupported (max " + std::to_string(kSceneSchemaVersion) + ")");
				REX::ERROR("SceneRegistry: '{}' declares scene schema {} but this build understands up to {} — skipped",
					fileName, schema, kSceneSchemaVersion);
				return;
			}

			std::vector<std::string> warnings;
			try {
				auto def = ParseScene(a_json, warnings);
				def.sourceFile = a_file;
				auto key = ToLower(def.id);
				if (const auto f = a_out.find(key); f != a_out.end()) {
					a_errors.push_back("[error] duplicate scene id '" + def.id + "' in '" + fileName +
						"' (already from '" + f->second.sourceFile.filename().string() + "') — keeping the first");
					REX::ERROR("SceneRegistry: duplicate scene id '{}' in '{}' — keeping first from '{}'",
						def.id, fileName, f->second.sourceFile.filename().string());
					return;
				}
				for (const auto& w : warnings) {
					a_errors.push_back("[warn] " + w);
					REX::WARN("SceneRegistry: {}", w);
				}
				REX::INFO("SceneRegistry: loaded scene '{}' ({} node(s)) from '{}'", def.id, def.nodes.size(), fileName);
				a_out[std::move(key)] = std::move(def);
			} catch (const std::exception& e) {
				a_errors.push_back("[error] '" + fileName + "': " + e.what());
				REX::ERROR("SceneRegistry: skipping scene in '{}': {}", fileName, e.what());
			}
		}
	}

	const SceneNode* SceneDef::FindNode(std::string_view a_id) const
	{
		const auto want = ToLower(std::string(a_id));
		for (const auto& n : nodes) {
			if (ToLower(n.id) == want) {
				return &n;
			}
		}
		return nullptr;
	}

	std::int32_t SceneDef::LinearStageOf(std::string_view a_nodeId) const
	{
		const auto want = ToLower(std::string(a_nodeId));
		for (std::size_t i = 0; i < linearStages.size(); i++) {
			if (ToLower(linearStages[i]) == want) {
				return static_cast<std::int32_t>(i);
			}
		}
		return -1;
	}

	SceneRegistry& SceneRegistry::GetSingleton()
	{
		static SceneRegistry singleton;
		return singleton;
	}

	void SceneRegistry::LoadAll()
	{
		namespace fs = std::filesystem;
		std::unordered_map<std::string, SceneDef> loaded;
		std::vector<std::string> errors;

		const fs::path dir = fs::current_path() / "Data" / "OSF";
		std::error_code ec;
		if (fs::is_directory(dir, ec)) {
			std::vector<fs::path> files;
			for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
				if (entry.is_regular_file(ec) && ToLower(entry.path().filename().string()).ends_with(".scene.json")) {
					files.push_back(entry.path());
				}
			}
			std::sort(files.begin(), files.end(), [](const fs::path& a_lhs, const fs::path& a_rhs) {
				return ToLower(a_lhs.filename().string()) < ToLower(a_rhs.filename().string());
			});
			for (const auto& file : files) {
				try {
					std::ifstream in(file, std::ios::binary);
					const auto j = nlohmann::json::parse(in, nullptr, true, true);  // tolerate // comments
					LoadSceneFile(j, file, loaded, errors);
				} catch (const std::exception& e) {
					errors.push_back("[error] '" + file.filename().string() + "': parse failed: " + e.what());
					REX::ERROR("SceneRegistry: failed to parse '{}': {}", file.filename().string(), e.what());
				}
			}
		}

		const auto sceneCount = loaded.size();
		const auto problemCount = errors.size();
		{
			std::unique_lock l{ lock };
			scenes = std::move(loaded);
			loadErrors = std::move(errors);
		}
		REX::INFO("SceneRegistry: {} scene(s) loaded, {} problem(s)", sceneCount, problemCount);
	}

	const SceneDef* SceneRegistry::Find(std::string_view a_id) const
	{
		std::shared_lock l{ lock };
		const auto it = scenes.find(ToLower(std::string(a_id)));
		return it != scenes.end() ? &it->second : nullptr;
	}

	size_t SceneRegistry::Size() const
	{
		std::shared_lock l{ lock };
		return scenes.size();
	}

	std::vector<std::string> SceneRegistry::LoadErrors() const
	{
		std::shared_lock l{ lock };
		return loadErrors;
	}
}
