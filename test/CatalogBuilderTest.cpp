#include "Check.h"
#include "API/CatalogBuilder.h"
#include "Registry/ContentRegistry.h"
#include "Serialization/ClipDurations.h"

namespace
{
	using namespace OSF;
	using Test::Check;
	using json = nlohmann::json;
	namespace Builder = API::CatalogBuilder;
	const std::filesystem::path kRoot{ "C:/catalog-test/Data/OSF" };

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
		const auto result = Builder::Build(registry, kRoot);
		if (!Check(result.schema == 2 && result.scenes.size() == 1, "builder preserves schema and all registered scenes")) return;
		const auto& scene = result.scenes.front();
		auto metadata = json(scene);
		metadata.erase("stages");
		const auto expected = json::parse(R"({
			"id":"Demo.Scene", "name":"Scene title", "species":"human", "pack":"Demo pack", "folder":"Poses/Seated",
			"sourceFile":"scenes.osf.json", "sourcePath":"Demo/scenes.osf.json", "sourceKind":"curatedAnimation",
			"placement":"followActor", "priority":-2, "weight":7, "actorCount":3,
			"unlisted":true, "curatedClip":true, "library":true, "clipsAvailable":false,
			"playerInputLock":false, "hideApparel":false, "fade":true, "tags":["Greeting","seated"],
			"roles":[{"name":"lead","gender":"female"},{"name":"partner","gender":"male"},{"name":"extra","gender":"any"}],
			"anchorKeywords":[291,1110], "anchorBaseForms":[1929]
		})");
		Check(metadata == expected, "schema-2 scene fields preserve authored identities, flags, roles and anchors without presentation filtering");
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
			{ Registry::CatalogSourceKind::kDerivedDebugAnimation, "derivedDebugAnimation" },
			{ Registry::CatalogSourceKind::kReferenceAnimation, "referenceAnimation" } }) {
			scene.sourceKind = kind;
			Check(Builder::Build(registry, {}).scenes.front().sourceKind == expected, "source taxonomy survives catalog conversion");
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
		Check(json(result.files.front()) == expected, "schema-2 file statistics and structured problem fields are preserved");
		Check(result.files[1].Rejected() && result.files[1].problems.size() == 13 &&
			result.files[1].problems.back().message == "Problem 12", "engine export never applies the browser's diagnostic truncation");
		Check(result.files[2].path.empty() && result.files[2].errors == 1, "cross-file diagnostics keep their empty path and ordering");
		Check(result.errors == std::vector<std::string>{ "first error", "second error" }, "plain load errors preserve their exact order");
		const auto owned = json(result);
		registry = {};
		Check(json(result) == owned, "file diagnostics and error text are owned by the returned catalog");
		const auto roundTrip = json::parse(owned.dump()).get<Catalog::Snapshot>();
		Check(json(roundTrip) == owned, "the exported snapshot round-trips through the released schema-2 value types");
	}
}

void CheckCatalogBuilder()
{
	Check(json(Builder::Build({}, {})) == json::parse(R"({"schema":2,"scenes":[],"files":[],"errors":[],"clips":[]})"),
		"empty registries still produce the complete schema-2 envelope");
	CheckScenesAndStages();
	CheckActorCountsAndPaths();
	CheckFileDiagnostics();
}
