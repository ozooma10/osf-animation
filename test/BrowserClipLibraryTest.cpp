#include "Check.h"
#include "API/BrowserClipLibrary.h"
#include <algorithm>
#include <array>

void RunBrowserClipLibraryTests()
{
	using OSF::Test::Check;
	using namespace OSF;
	Catalog::ClipSourceSnapshot source;
	Catalog::ClipSourceNode scene;
	scene.id = "demo.scene"; scene.sourceFile = "demo.osf.json"; scene.pack = "Demo";
	source.nodes.push_back(scene);
	source.sourceClips.push_back({scene.id, "clips/wave.glb", "wave", "clips/wave.glb", "human", 2, 3, 4.0f, true});
	const auto initial = API::BrowserClipLibrary::Build(source);
	Check(initial.size() == 1 && initial[0].source == scene.id && initial[0].stage == 2 && initial[0].clip == 3,
		"browser preserves the exact source clip address across global nodes");
	if (initial.empty()) return;
	Check(initial[0].id == "osf.scene-clip/0faf6e8d04dd8e88", "browser preserves the legacy generated ID");
	Check(initial[0].name == "wave.glb · wave" && initial[0].tags == std::vector<std::string>{"scene.clip"} &&
		initial[0].unlisted && initial[0].library && initial[0].followActor && initial[0].clearOffset &&
		!initial[0].playerInputLock && !initial[0].hideApparel && !initial[0].fade,
		"inspection labels and playback defaults belong to Browser");
	const auto checkJson = [](const Catalog::ClipSourceSnapshot& value) {
		const auto text = nlohmann::json(value).dump();
		Check(nlohmann::json(API::BrowserClipLibrary::BuildFromCatalogJson(text.c_str())) ==
			nlohmann::json(API::BrowserClipLibrary::Build(value)), "provider JSON projection preserves all generated records");
	};
	checkJson(source);
	auto duplicate = source.sourceClips.front(); duplicate.stage = 5;
	source.sourceClips.insert(source.sourceClips.begin(), duplicate);
	Check(API::BrowserClipLibrary::Build(source).front().stage == 2, "dedup winner follows authored source order rather than callback order");
	auto reserved = scene; reserved.id = initial[0].id; source.nodes.push_back(reserved);
	Check(API::BrowserClipLibrary::Build(source).front().id == initial[0].id + "-1", "generated IDs never shadow authored IDs");
	checkJson(source);
	auto curated = scene; curated.id = "curated"; curated.library = curated.curatedClip = true;
	source.nodes.push_back(curated); duplicate.source = curated.id; source.sourceClips.push_back(duplicate);
	Check(API::BrowserClipLibrary::Build(source).empty(), "explicit clip registrations win over harvested copies in the same pack");
	checkJson(source);
	// Source precedence is filename, case-insensitive scene ID, then exact stage/clip address.
	// Different input enumeration orders must preserve the complete record, including its source.
	Catalog::ClipSourceSnapshot ordered;
	for (const auto& [id, file] : std::vector<std::pair<std::string, std::string>>{
		{"z.scene", "A.osf.json"}, {"a.scene", "b.osf.json"}, {"B.scene", "a.osf.json"}}) {
		auto item = scene; item.id = id; item.sourceFile = file; ordered.nodes.push_back(item);
		for (const auto address : {std::array{1u, 0u}, std::array{0u, 1u}, std::array{0u, 0u}}) {
			auto clip = source.sourceClips.front(); clip.source = id;
			clip.stage = address[0]; clip.clip = address[1]; ordered.sourceClips.push_back(clip);
		}
	}
	for (int pass = 0; pass < 4; ++pass) {
		const auto entries = API::BrowserClipLibrary::Build(ordered);
		Check(entries.size() == 1 && entries[0].source == "B.scene" &&
			entries[0].stage == 0 && entries[0].clip == 0 && entries[0].id == initial[0].id,
			"deduplication preserves every source-order tie breaker and the stable ID");
		checkJson(ordered);
		std::rotate(ordered.sourceClips.begin(), ordered.sourceClips.begin() + 5, ordered.sourceClips.end());
		std::reverse(ordered.nodes.begin(), ordered.nodes.end());
	}
	// Same path but different animation IDs remain distinct, even if only their case differs.
	auto secondAnimation = ordered.sourceClips.front(); secondAnimation.animId = "Wave";
	ordered.sourceClips.push_back(secondAnimation);
	Check(API::BrowserClipLibrary::Build(ordered).size() == 2, "animation IDs are not case-folded during deduplication");
	checkJson(ordered);
	// Rich, irrelevant sections (and future fields) are intentionally not materialized by Browser.
	auto rich = nlohmann::json(ordered);
	rich["future"] = {{"nodes", {{{"id", "ignored"}}}}};
	rich["nodes"][0]["stages"] = nlohmann::json::array({{{"tags", {"player.emote.fake"}}}});
	rich["files"] = nlohmann::json::array({{{"problems", {{{"message", "ignored"}}}}}});
	Check(nlohmann::json(API::BrowserClipLibrary::BuildFromCatalogJson(rich.dump().c_str())) ==
		nlohmann::json(API::BrowserClipLibrary::Build(ordered)), "unused nested fields do not affect provider input projection");
	for (const auto field : {"schema", "nodes", "sourceClips"}) {
		auto malformed = rich; malformed[field] = nullptr;
		bool rejected = false;
		try { (void)API::BrowserClipLibrary::BuildFromCatalogJson(malformed.dump().c_str()); } catch (...) { rejected = true; }
		Check(rejected, "malformed required provider sections are rejected");
	}
	rich["nodes"][0]["tags"] = false;
	bool rejected = false;
	try { (void)API::BrowserClipLibrary::BuildFromCatalogJson(rich.dump().c_str()); } catch (...) { rejected = true; }
	Check(rejected, "malformed policy fields cannot silently admit a scene");
	source.nodes.resize(1); source.sourceClips.resize(1);
	source.nodes[0].tags = {"Player.Emote.Wave"};
	Check(API::BrowserClipLibrary::Build(source).empty(), "emote suppression is browser policy and case-insensitive");
	checkJson(source);
	source.nodes[0].tags.clear(); source.nodes[0].library = true;
	Check(API::BrowserClipLibrary::Build(source).empty(), "reference libraries are not harvested recursively");
	checkJson(source);
	source.nodes[0].library = false; source.sourceClips[0].available = false;
	Check(API::BrowserClipLibrary::Build(source).empty(), "unavailable clips cannot become inspection entries");
	checkJson(source);
	Catalog::Snapshot decorated;
	Catalog::Node provided; provided.provider = API::BrowserClipLibrary::kOwner;
	provided.sourcePath = "demo.osf.json"; decorated.nodes.push_back(provided);
	Catalog::File file; file.path = "demo.osf.json"; file.clipEntries = 2; decorated.files.push_back(file);
	API::BrowserClipLibrary::Decorate(decorated);
	Check(decorated.nodes[0].sourceKind == "derivedDebugAnimation" && decorated.files[0].clipEntries == 3,
		"browser decorates provider origins and import-panel counts locally");
}
