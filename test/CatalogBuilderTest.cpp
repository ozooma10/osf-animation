#include "Check.h"
#include "API/CatalogBuilder.h"
#include "API/CatalogJson.h"
#include "API/ClipProviderBridge.h"
#include "Registry/ContentRegistry.h"
#include "Serialization/ClipDurations.h"
#include <bit>
#include <limits>

namespace
{
	using namespace OSF;
	using Test::Check;
	using json = nlohmann::json;
	namespace Builder = API::CatalogBuilder;
	const std::filesystem::path kRoot{ "C:/catalog-test/Data/OSF" };

	void CheckSerialized(const Catalog::Snapshot& snapshot)
	{
		const auto expected = json(snapshot).dump(-1, ' ', false, json::error_handler_t::replace);
		const auto actual = API::CatalogJson::Serialize(snapshot);
		Check(actual.size() == expected.size() && json::parse(actual) == json::parse(expected),
			"streamed schema-3 JSON preserves every value, array order, encoding policy and compact payload size");
	}

	Registry::SceneDef MakeScene()
	{
		Registry::SceneDef def;
		def.id = "Demo.Scene";
		def.name = "Scene title";
		def.species = "human";
		def.pack = "Demo pack";
		def.folder = "Poses/Seated";
		def.sourceFile = kRoot / "Demo/scenes.osf.json";
		def.sourceKind = Registry::CatalogSourceKind::kCuratedAnimation;
		def.worldPlacement = Animation::WorldPlacementMode::kFollowActor;
		def.priority = -2;
		def.weight = 7;
		def.unlisted = def.curatedClip = def.library = def.fade = true;
		def.clipsAvailable = def.playerInputLock = def.hideApparel = false;
		def.tags = { "Greeting", "seated" };
		def.anchorKeywords = { 0x123, 0x456 };
		def.anchorBaseForms = { 0x789 };
		def.roles = { { .name = "lead", .gender = Registry::RoleGender::kFemale },
			{ .name = "partner", .gender = Registry::RoleGender::kMale }, { .name = "extra" } };
		Registry::StageDef authored;
		authored.name = "Authored duration";
		authored.tags = { "loop-tag" };
		authored.timer = 99; // Catalog timing comes from the node, not the stage's authoring fields.
		authored.loops = 88;
		authored.hold = 0.25f;
		authored.clips = { { .file = "naf:builder-authored.glb", .sec = 2.0f },
			{ .file = "second.glb", .sec = 300.0f } };
		Registry::SceneNode loop;
		loop.id = "loop";
		loop.stages = { authored, {} }; // Only the first stage is exported for a linear node.
		loop.loopMode = Registry::LoopMode::kCount;
		loop.loopCount = 4;
		loop.timerSec = 13;
		Registry::SceneNode cached;
		cached.id = "cached";
		cached.loopMode = Registry::LoopMode::kHold;
		cached.stages.push_back({ .name = "Cached duration", .clips = {
			{ .file = "Data/NAF/builder-cached.glb", .animId = "named" } } });
		Registry::SceneNode noClips;
		noClips.id = "no-clips";
		noClips.stages.emplace_back();
		noClips.timerSec = 5;
		Registry::SceneNode uncached;
		uncached.id = "uncached";
		uncached.stages.push_back({ .clips = { { .file = "builder-unknown.glb" } } });
		def.nodes = { loop, cached, noClips, uncached, { .id = "empty-node" } };
		def.entry = "cached";
		def.linearStages = { "LOOP", "cached", "no-clips", "uncached", "missing-node", "empty-node" };
		return def;
	}

	void CheckScenesAndStages()
	{
		Serialization::ClipDurations::Record("naf:builder-authored.glb", "", 20.0f);
		Serialization::ClipDurations::Record("naf:builder-cached.glb", "", 99.0f);
		Serialization::ClipDurations::Record("naf:builder-cached.glb", "named", 4.25f);
		Registry::ContentRegistrySnapshot registry;
		registry.scenes.emplace("demo.scene", MakeScene());
		registry.clipAvailability["Data/NAF/builder-cached.glb"] = true;
        registry.scenes.at("demo.scene").roles.resize(1);
        auto& independent = registry.scenes.at("demo.scene").nodes[1];
        independent.tags = {"GREETING", "only-this-node"}; independent.priority = 9; independent.weight = 3; independent.unlisted = false;
        const auto node = Builder::BuildNode(registry, registry.scenes.at("demo.scene"), independent);
        Check(node.scene == "Demo.Scene" && node.id == "cached" && node.entry && node.stage == 1 && node.clipsAvailable,
            "an available node is independently addressable despite unavailable route siblings");
        Check(node.tags == std::vector<std::string>{"Greeting", "seated", "only-this-node"} && node.priority == 9 && node.weight == 3 && !node.unlisted,
            "node tags union case-insensitively and ranking overrides route defaults");
        Check(!Builder::BuildNode(registry, registry.scenes.at("demo.scene"), registry.scenes.at("demo.scene").nodes[0]).clipsAvailable,
            "missing clips and inconsistent segment casts cannot match");
        registry.scenes.at("demo.scene") = MakeScene();
		const auto result = Builder::Build(registry, kRoot);
		CheckSerialized(result);
        Check(result.nodes.size() == 5 && result.nodes[1].id == "cached", "all nodes publish independently including nonlinear and unavailable nodes");
		Check(result.sourceClips.size() == 4 && result.sourceClips[2].source == "Demo.Scene" &&
			result.sourceClips[2].node == 1 && result.sourceClips[2].stage == 0 && result.sourceClips[2].clip == 0 &&
			result.sourceClips[2].animId == "named" && result.sourceClips[2].available && !result.sourceClips[0].available,
			"source metadata preserves exact inline clip addresses and per-clip availability even in a partially unavailable scene");
		if (!Check(result.schema == 3 && result.scenes.size() == 1, "builder preserves schema and all registered scenes")) return;
		const auto& scene = result.scenes.front();
		auto metadata = json(scene);
		metadata.erase("stages");
		const auto expected = json::parse(R"({
			"provider":"", "id":"Demo.Scene", "name":"Scene title", "species":"human", "pack":"Demo pack", "folder":"Poses/Seated",
			"sourceFile":"scenes.osf.json", "sourcePath":"Demo/scenes.osf.json", "sourceKind":"curatedAnimation",
			"placement":"followActor", "priority":-2, "weight":7, "actorCount":3,
			"unlisted":true, "curatedClip":true, "library":true, "clipsAvailable":false,
			"playerInputLock":false, "hideApparel":false, "fade":true, "tags":["Greeting","seated"],
			"roles":[{"name":"lead","gender":"female"},{"name":"partner","gender":"male"},{"name":"extra","gender":"any"}],
			"anchorKeywords":[291,1110], "anchorBaseForms":[1929]
		})");
		Check(metadata == expected, "schema-3 scene fields preserve authored identities, flags, roles and anchors without presentation filtering");
		auto expectedStages = json::parse(R"([
			{"index":0,"clipCount":2,"loopCount":4,"name":"Authored duration","loopMode":"count","tags":["loop-tag"],"loopSec":2,"timerSec":13,"hold":0.25,"resolved":true},
			{"index":1,"clipCount":1,"loopCount":0,"name":"Cached duration","loopMode":"hold","tags":[],"loopSec":4.25,"timerSec":0,"hold":-1,"resolved":true},
			{"index":2,"clipCount":0,"loopCount":0,"name":"","loopMode":"once","tags":[],"loopSec":-1,"timerSec":5,"hold":-1,"resolved":true},
			{"index":3,"clipCount":1,"loopCount":0,"name":"","loopMode":"once","tags":[],"loopSec":-1,"timerSec":0,"hold":-1,"resolved":true},
			{"index":4,"clipCount":0,"loopCount":0,"name":"","loopMode":"","tags":[],"loopSec":-1,"timerSec":0,"hold":-1,"resolved":false},
			{"index":5,"clipCount":0,"loopCount":0,"name":"","loopMode":"","tags":[],"loopSec":-1,"timerSec":0,"hold":-1,"resolved":false}
		])");
        for (auto& stage : expectedStages) stage["durationKey"] = "";
        expectedStages[1]["durationKey"] = Serialization::ClipDurations::KeyFor("naf:builder-cached.glb", "named");
        expectedStages[3]["durationKey"] = Serialization::ClipDurations::KeyFor("builder-unknown.glb", "");
		Check(json(scene.stages) == expectedStages,
			"linear stages preserve order, case-insensitive resolution, node timing, first-clip duration precedence and unresolved slots");
		const auto owned = json(result);
		registry.scenes.clear();
		Serialization::ClipDurations::Record("naf:builder-cached.glb", "named", 8.0f);
		Check(json(result) == owned, "returned catalog owns its nested scene data and duration values");
		Check(result.clips.empty(), "unavailable scenes do not request background probes");
		auto def=MakeScene();
		def.clipsAvailable=true;
		def.linearStages.clear();
		registry.scenes.emplace("nonlinear",def);
		registry.scenes.emplace("duplicate",def);
		const auto nonlinear=Builder::Build(registry,kRoot);
		Check(nonlinear.clips.size() == 2, "probe references include nonlinear nodes, deduplicate clips, and skip authored durations");
	}

	void CheckActorCountsAndPaths()
	{
		auto def = MakeScene();
		Check(Builder::ActorCount(def) == 3, "explicit roles take precedence over clip count");
		def.roles.clear();
		Check(Builder::ActorCount(def) == 1, "untyped cast count comes from the entry node");
		def.entry = "missing";
		Check(Builder::ActorCount(def) == 2, "missing entry falls back to the first node");
		def.entry = "empty-node";
		Check(Builder::ActorCount(def) == 0, "an existing empty entry does not fall back to another node");
		def.nodes.clear();
		Check(Builder::ActorCount(def) == 0, "a scene without roles or stages has no inferred cast");

		Registry::ContentRegistrySnapshot registry;
		auto& scene = registry.scenes["path-test"];
		scene.worldPlacement = Animation::WorldPlacementMode::kAnchorAndPin;
		const std::vector<std::pair<std::filesystem::path, std::string>> paths = {
			{ kRoot / "nested/demo.osf.json", "nested/demo.osf.json" },
			{ kRoot.parent_path() / "private.osf.json", "private.osf.json" },
			{ "D:/other-drive/external.osf.json", "external.osf.json" },
			{ "relative.osf.json", "relative.osf.json" }, { {}, "" }
		};
		for (const auto& [path, expected] : paths) {
			scene.sourceFile = path;
			const auto result = Builder::Build(registry, kRoot).scenes.front();
			Check(result.sourcePath == expected, "catalog paths stay relative or fall back to the filename");
			Check(result.placement == "anchorAndPin" && result.stages.empty(), "default placement and nonlinear scenes retain their metadata");
		}
		scene.sourceFile = kRoot / "nested/demo.osf.json";
		Check(Builder::Build(registry, {}).scenes.front().sourcePath == "demo.osf.json", "unavailable content root uses the filename fallback");
		scene.sourceFile = std::filesystem::path{ u8"C:/private/caf\u00e9.osf.json" };
		Check(json(Builder::Build(registry, {}).scenes.front())["sourceFile"] == "caf\xC3\xA9.osf.json", "filename fallback preserves UTF-8");
		for (const auto& [kind, expected] : std::vector<std::pair<Registry::CatalogSourceKind, std::string>>{
			{ Registry::CatalogSourceKind::kAuthoredScene, "authoredScene" },
			{ Registry::CatalogSourceKind::kCuratedAnimation, "curatedAnimation" },
			{ Registry::CatalogSourceKind::kProvidedScene, "providedScene" },
			{ Registry::CatalogSourceKind::kReferenceAnimation, "referenceAnimation" } }) {
			scene.sourceKind = kind;
			Check(Builder::Build(registry, {}).scenes.front().sourceKind == expected, "source taxonomy survives catalog conversion");
		}
	}

	void CheckRepeatedClips()
	{
		Registry::ContentRegistrySnapshot registry;
		auto def = MakeScene();
		const auto file = def.nodes[0].stages[0].clips[0].file;
		def.nodes[0].stages[0].clips = {{.file = file, .animId = "One", .sec = 2},
			{.file = file, .animId = "one", .sec = 7}};
		registry.scenes.emplace("demo.scene", def);
		registry.clipAvailability[file] = true;
		const auto first = Builder::Build(registry, kRoot);
		if (!Check(first.sourceClips.size() >= 2, "repeated file still exports each source address")) return;
		const auto& a = first.sourceClips[0];
		const auto& b = first.sourceClips[1];
		Check(a.file == file && b.file == file && a.display == b.display && a.species == b.species &&
			a.animId == "One" && b.animId == "one" && a.seconds == 2 && b.seconds == 7 &&
			a.clip == 0 && b.clip == 1 && a.available && b.available,
			"shared lexical metadata preserves per-occurrence animation, duration, availability and address");
		registry.clipAvailability[file] = false;
		const auto second = Builder::Build(registry, kRoot);
		Check(!second.sourceClips[0].available && first.sourceClips[0].available,
			"a new publication observes current availability without mutating a previous catalog");
	}

	void CheckProviderInputs()
	{
		Registry::ContentRegistrySnapshot registry;
		auto def = MakeScene();
		registry.scenes.emplace("demo.scene", def);
		Registry::SceneDef empty; empty.id = "Reserved.NoClips"; empty.sourceFile = kRoot / "other.osf.json";
		registry.scenes.emplace("reserved.noclips", empty);
		const auto full = Builder::Build(registry, {});
		CheckSerialized(full);
		const auto compact = Builder::BuildClipSources(registry);
		Check(compact.scenes.size() == full.scenes.size() && json(compact.sourceClips) == json(full.sourceClips),
			"compact provider input preserves all source addresses, including unavailable clips and nonlinear nodes");
		for (std::size_t i = 0; i < compact.scenes.size(); ++i) {
			const auto expected = json(full.scenes[i]);
			const auto projected = json(compact.scenes[i]);
			for (const auto& [key, value] : projected.items())
				Check(expected.at(key) == value, "compact scene metadata matches the full catalog, including reserved IDs without clips");
		}
		struct State { bool compact; int replies; bool malformed; };
		const auto callback = [](const char* input, API::TextCallback emit, void* output, void* context) {
			const auto& state = *static_cast<State*>(context);
			const auto data = json::parse(input);
			Check(data.at("schema") == (state.compact ? 1 : 3) && data.contains("files") != state.compact,
				"registration kind selects its documented input schema");
			Check(data.at("scenes").size() == 2, "the production bridge supplies the entire base scene namespace");
			API::ClipScene entry; entry.id = "provided"; entry.source = "Demo.Scene";
			const auto response = state.malformed ? std::string("null") : json(std::vector{entry}).dump();
			for (int i = 0; i < state.replies; ++i) emit(response.c_str(), output);
		};
		for (const bool compactInput : {false, true}) {
			State valid{compactInput, 1, false};
			const auto result = API::ClipProviderBridge::Invoke(registry, callback, &valid, compactInput);
			Check(result.size() == 1 && result[0].id == "provided" && result[0].source == "Demo.Scene",
				"full and compact registrations share the same owned response decoder");
			for (auto invalid : {State{compactInput, 0, false}, State{compactInput, 2, false}, State{compactInput, 1, true}}) {
				bool rejected = false;
				try { (void)API::ClipProviderBridge::Invoke(registry, callback, &invalid, compactInput); } catch (...) { rejected = true; }
				Check(rejected, "provider response must be one well-formed scene array in both input modes");
			}
		}
	}

	void CheckFileDiagnostics()
	{
		Registry::ContentRegistrySnapshot registry;
		Registry::ContentFileStats file;
		file.file = "content.osf.json"; file.path = "pack/content.osf.json"; file.pack = "Pack"; file.library = true;
		file.schema = 3; file.bytes = 123456; file.parseMs = 1.25f;
		file.scenes = 2; file.declaredScenes = 9; file.hidden = 1; file.rejectedScenes = 7;
		file.unlisted = 3; file.anchored = 4; file.nodes = 5; file.stages = 6; file.roles = 8;
		file.clips = 10; file.distinctClips = 11; file.missingClips = 12;
		file.cues = 14; file.actions = 15; file.sounds = 16; file.cameras = 17; file.clipEntries = 18;
		file.errors = 19; file.warnings = 20;
		file.species = { "human", "robot" }; file.missingClipExamples = { "missing.glb" };
		file.problems.push_back({ true, "missing", "Full message", "Install clip", "scene", "node", "role", "missing.glb" });
		registry.files.push_back(file);
		const auto expected = json::parse(R"({
			"file":"content.osf.json","path":"pack/content.osf.json","pack":"Pack","library":true,
			"schema":3,"bytes":123456,"parseMs":1.25,"scenes":2,"declaredScenes":9,"hidden":1,"rejectedScenes":7,
			"unlisted":3,"anchored":4,"nodes":5,"stages":6,"roles":8,"clips":10,"distinctClips":11,"missingClips":12,
			"cues":14,"actions":15,"sounds":16,"cameras":17,"clipEntries":18,"errors":19,"warnings":20,
			"species":["human","robot"],"missingClipExamples":["missing.glb"],
			"problems":[{"warning":true,"code":"missing","message":"Full message","hint":"Install clip","scene":"scene","node":"node","role":"role","clip":"missing.glb"}]
		})");
		Registry::ContentFileStats rejected;
		rejected.file = rejected.path = "rejected.osf.json";
		rejected.errors = 13;
		for (int i = 0; i < 13; ++i) rejected.problems.push_back({ .message = "Problem " + std::to_string(i) });
		registry.files.push_back(rejected);
		registry.files.push_back({ .errors = 1 }); // Cross-file bucket has no source path.
		registry.loadErrors = { "first error", "second error" };
		const auto result = Builder::Build(registry, kRoot);
		if (!Check(result.files.size() == 3 && result.scenes.empty(), "files survive even when no scenes were accepted")) return;
		Check(json(result.files.front()) == expected, "schema-3 file statistics and structured problem fields are preserved");
		Check(result.files[1].Rejected() && result.files[1].problems.size() == 13 &&
			result.files[1].problems.back().message == "Problem 12", "engine export never applies the browser's diagnostic truncation");
		Check(result.files[2].path.empty() && result.files[2].errors == 1, "cross-file diagnostics keep their empty path and ordering");
		Check(result.errors == std::vector<std::string>{ "first error", "second error" }, "plain load errors preserve their exact order");
		const auto owned = json(result);
		registry = {};
		Check(json(result) == owned, "file diagnostics and error text are owned by the returned catalog");
		const auto roundTrip = json::parse(owned.dump()).get<Catalog::Snapshot>();
		Check(json(roundTrip) == owned, "the exported snapshot round-trips through the released schema-3 value types");
		CheckSerialized(result);
	}

	void CheckSerializationEdges()
	{
		Catalog::Snapshot snapshot;
		CheckSerialized(snapshot);
		// Exercise every nested record, empty/nonempty arrays and all scalar types.
		snapshot.scenes.resize(1);
		auto& scene = snapshot.scenes.front();
		scene.roles.push_back({"lead", "any"});
		scene.stages.resize(1);
		scene.priority = std::numeric_limits<std::int32_t>::min();
		scene.weight = std::numeric_limits<std::int32_t>::max();
		scene.actorCount = std::numeric_limits<std::uint32_t>::max();
		scene.anchorKeywords = {0, std::numeric_limits<std::uint32_t>::max()};
		scene.anchorBaseForms = {42};
		snapshot.files.resize(1);
		auto& file = snapshot.files.front();
		file.schema = std::numeric_limits<std::int64_t>::min();
		file.bytes = std::numeric_limits<std::uint64_t>::max();
		file.problems.resize(1);
		file.species = {"human"};
		file.missingClipExamples = {"missing"};
		snapshot.clips.push_back({"key", "file", "anim"});
		snapshot.sourceClips.resize(1);
		snapshot.nodes.resize(1);
		snapshot.nodes.front().tags = {"one", "two"};
		std::string controls;
		for (int i = 0; i < 32; ++i) controls += static_cast<char>(i);
		std::vector<std::string> strings = {"", "quote\" slash/ backslash\\", controls,
			"\xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80", std::string(32768, '\n'),
			"\xE2\x82", "\xED\xA0\x80", "\xF0\x80\x80\x80", "\xF4\x90\x80\x80"};
		// Isolated bytes include all continuation, invalid lead and truncated lead cases.
		for (int i = 0; i < 256; ++i) strings.emplace_back(1, static_cast<char>(i));
		for (const auto& value : strings) {
			scene.name = scene.roles.front().name = scene.stages.front().name = value;
			scene.tags = {value};
			file.path = file.problems.front().message = value;
			snapshot.clips.front().file = snapshot.sourceClips.front().file = value;
			snapshot.nodes.front().id = value;
			snapshot.errors = {value, "second"};
			CheckSerialized(snapshot);
		}
		for (float value : {0.0f, -0.0f, 1.0f / 3, std::numeric_limits<float>::denorm_min(),
			std::numeric_limits<float>::min(), std::numeric_limits<float>::max(),
			std::numeric_limits<float>::lowest(), std::numeric_limits<float>::infinity(),
			-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
			scene.stages.front().loopSec = file.parseMs = snapshot.sourceClips.front().seconds = value;
			CheckSerialized(snapshot);
		}
		// Deterministic bit patterns exercise exponent/rounding boundaries without a new formatter.
		std::uint32_t bits = 0x12345678;
		for (int i = 0; i < 512; ++i) {
			bits = bits * 1664525u + 1013904223u;
			file.parseMs = std::bit_cast<float>(bits);
			CheckSerialized(snapshot);
		}
		const auto saved = API::CatalogJson::Serialize(snapshot);
		const auto copy = saved;
		snapshot = {};
		(void)API::CatalogJson::Serialize(snapshot);
		Check(saved == copy, "serialized documents own their bytes across later reads and source destruction");
	}
}

void CheckCatalogBuilder()
{
	Check(json(Builder::Build({}, {})) == json::parse(R"({"schema":3,"scenes":[],"files":[],"errors":[],"clips":[],"sourceClips":[],"nodes":[]})"),
		"empty registries still produce the complete schema-3 envelope");
	CheckScenesAndStages();
	CheckActorCountsAndPaths();
	CheckRepeatedClips();
	CheckProviderInputs();
	CheckFileDiagnostics();
	CheckSerializationEdges();
}
