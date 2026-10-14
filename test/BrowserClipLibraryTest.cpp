#include "Check.h"
#include "API/BrowserClipLibrary.h"
#include <algorithm>

void RunBrowserClipLibraryTests()
{
	using OSF::Test::Check;
	using namespace OSF;
	Catalog::Snapshot source;
	Catalog::Scene scene;
	scene.id = "demo.scene"; scene.sourceFile = "demo.osf.json"; scene.pack = "Demo";
	source.scenes.push_back(scene);
	source.sourceClips.push_back({scene.id, "clips/wave.glb", "wave", "clips/wave.glb", "human", 1, 2, 3, 4.0f, true});
	const auto initial = API::BrowserClipLibrary::Build(source);
	Check(initial.size() == 1 && initial[0].source == scene.id && initial[0].node == 1 && initial[0].stage == 2 && initial[0].clip == 3,
		"browser preserves the exact source clip address across nonlinear nodes");
	if (initial.empty()) return;
	Check(initial[0].id == "osf.scene-clip/0faf6e8d04dd8e88", "browser preserves the legacy generated ID");
	Check(initial[0].name == "wave.glb · wave" && initial[0].tags == std::vector<std::string>{"scene.clip"} &&
		initial[0].unlisted && initial[0].library && initial[0].followActor && initial[0].clearOffset &&
		!initial[0].playerInputLock && !initial[0].hideApparel && !initial[0].fade,
		"inspection labels and playback defaults belong to Browser");
	auto duplicate = source.sourceClips.front(); duplicate.node = 5;
	source.sourceClips.insert(source.sourceClips.begin(), duplicate);
	Check(API::BrowserClipLibrary::Build(source).front().node == 1, "dedup winner follows authored source order rather than callback order");
	auto reserved = scene; reserved.id = initial[0].id; source.scenes.push_back(reserved);
	Check(API::BrowserClipLibrary::Build(source).front().id == initial[0].id + "-1", "generated IDs never shadow authored IDs");
	auto curated = scene; curated.id = "curated"; curated.library = curated.curatedClip = true;
	source.scenes.push_back(curated); duplicate.source = curated.id; source.sourceClips.push_back(duplicate);
	Check(API::BrowserClipLibrary::Build(source).empty(), "explicit clip registrations win over harvested copies in the same pack");
	source.scenes.resize(1); source.sourceClips.resize(1);
	source.scenes[0].tags = {"Player.Emote.Wave"};
	Check(API::BrowserClipLibrary::Build(source).empty(), "emote suppression is browser policy and case-insensitive");
	source.scenes[0].tags.clear(); source.scenes[0].library = true;
	Check(API::BrowserClipLibrary::Build(source).empty(), "reference libraries are not harvested recursively");
	source.scenes[0].library = false; source.sourceClips[0].available = false;
	Check(API::BrowserClipLibrary::Build(source).empty(), "unavailable clips cannot become inspection entries");
	source.scenes[0].provider = API::BrowserClipLibrary::kOwner;
	source.scenes[0].sourcePath = "demo.osf.json";
	Catalog::File file; file.path = "demo.osf.json"; file.clipEntries = 2; source.files.push_back(file);
	API::BrowserClipLibrary::Decorate(source);
	Check(source.scenes[0].sourceKind == "derivedDebugAnimation" && source.files[0].clipEntries == 3,
		"browser decorates provider origins and import-panel counts locally");
}
