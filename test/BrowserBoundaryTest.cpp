#include "Check.h"
#include "API/EngineClient.h"
#include "API/UIBridgeCatalog.h"
#include "API/BrowserSessionQueue.h"
#include "API/EngineServiceState.h"
#include "API/BrowserForwarder.h"
#include "Serialization/WheelPins.h"
#include <atomic>
#include <future>
#include <thread>

// Wheel presentation tests use isolated storage, never the player's wheel-pins.json.
namespace OSF::Serialization::WheelPins
{
	bool customized{};
	std::vector<Entry> entries;
	bool Customized() { return customized; }
	std::vector<Entry> Entries() { return entries; }
}

namespace
{
	OSF::Catalog::Snapshot catalog;
	int reads{};
	bool ReadCatalog(OSF::API::TextCallback callback, void* context)
	{
		++reads;
		const auto text=nlohmann::json(catalog).dump();
		callback(text.c_str(),context);
		return true;
	}
}

int main()
{
	using OSF::Test::Check;
	using namespace OSF::API;
	namespace Queue=OSF::Util::NativeMainThreadQueue;
	Check(RequestSceneAPI() == nullptr && RequestEngineAPI() == nullptr, "absent engine discovery is normal");
	Check(!OpenBrowser(), "Papyrus-compatible forwarding is false with no addon DLL");
	Check(!EngineClient::Connect(nullptr) && !EngineClient::Post([]{}), "absent provider refuses posts");
	OSFEngineAPI api{};
	api.size=sizeof(api); api.version=kOSFEngineAPIVersion+1;
	Check(!EngineClient::Connect(&api), "unsupported services version refused");
	api.version=kOSFEngineAPIVersion; api.size=8;
	Check(!EngineClient::Connect(&api), "truncated table refused before use");
	api.size=sizeof(api); api.ReadCatalog=ReadCatalog;
	Check(EngineClient::Connect(&api), "complete public services table accepted");

	EngineServiceState engine;
	struct Result { int executed{}, disposed{}; } result;
	const auto callback=[](void* p,bool execute) {
		auto& r=*static_cast<Result*>(p); ++r.disposed; if (execute) ++r.executed;
	};
	Check(!engine.Post(nullptr,nullptr), "null frame callback rejected");
	Check(engine.Post(callback,&result) && result.disposed == 0, "accepted callback is always deferred");
	engine.ResetWorld();
	engine.Post(callback,&result);
	Queue::Drain();
	Check(result.disposed == 2 && result.executed == 1, "old-world callback disposes once; new-world callback runs");
	Queue::Post([&] { engine.ResetWorld(); });
	engine.Post(callback,&result);
	Queue::Drain();
	Check(result.disposed == 3 && result.executed == 1, "reset during the current drain cancels later commands in that batch");

	BrowserSessionQueue session([&](std::function<void()> task) {
		auto* owned=new std::function<void()>(std::move(task));
		engine.Post([](void* p,bool execute) {
			std::unique_ptr<std::function<void()>> work(static_cast<std::function<void()>*>(p));
			if (execute) (*work)();
		},owned);
	});
	int actions=0, cleanup=0;
	const auto token=engine.Subscribe([&](std::uint32_t event) { if (event == 1) session.Reset([&] { ++cleanup; }); });
	session.Visibility(true,[]{}); Queue::Drain();
	session.Post([&] { ++actions; });
	session.Post([&] { ++actions; },false); // a pending open
	engine.ResetWorld(); Queue::Drain();
	Check(actions == 0 && cleanup == 1, "world notification cancels visible commands and pending opens before cleanup");
	session.Visibility(true,[]{}); session.Post([&] { ++actions; }); Queue::Drain();
	Check(actions == 1, "new session works after world replacement");
	Check(engine.Unsubscribe(token) && !engine.Unsubscribe(token), "notification removal is idempotently rejected when stale");
	std::uint64_t self{};
	int calls=0;
	self=engine.Subscribe([&](std::uint32_t) { ++calls; engine.Unsubscribe(self); });
	engine.Notify(2); engine.Notify(2);
	Check(calls == 1, "callback can unsubscribe itself");
	const auto throws=engine.Subscribe([](std::uint32_t) { throw 1; });
	const auto survivor=engine.Subscribe([&](std::uint32_t) { ++calls; });
	engine.Notify(2);
	Check(calls == 2, "throwing observer does not block another observer");
	engine.Unsubscribe(throws); engine.Unsubscribe(survivor);
	std::promise<void> entered, finish;
	auto finished=finish.get_future();
	const auto concurrent=engine.Subscribe([&](std::uint32_t) { entered.set_value(); finished.wait(); });
	std::thread dispatch([&] { engine.Notify(2); });
	entered.get_future().wait();
	auto remove=std::async(std::launch::async,[&] { return engine.Unsubscribe(concurrent); });
	Check(remove.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout, "unsubscription waits for another thread's callback");
	finish.set_value(); dispatch.join(); Check(remove.get(), "unsubscription completes after callback returns");

	OSF::Catalog::Scene emote;
	emote.id="custom.wave"; emote.name="Wave"; emote.tags={"Player.Emote.Wave"}; emote.actorCount=1;
	emote.roles.push_back({"actor","any"}); emote.placement="followActor";
	emote.stages.push_back({.index=0,.clipCount=1,.loopCount=2,.name="Wave loop",.loopMode="count",.tags={"wave"},.loopSec=3.0f,.resolved=true});
	catalog.scenes.push_back(emote);
	auto library=emote; library.id="library.wave"; library.library=true; library.curatedClip=true;
	library.name="Library wave"; library.stages.front().hold=0.5f; library.stages.front().timerSec=4.0f;
	catalog.scenes.push_back(library);
	auto missing=emote; missing.id="missing.wave"; missing.clipsAvailable=false; catalog.scenes.push_back(missing);
	OSF::Catalog::File file; file.path="broken.osf.json"; file.file=file.path; file.errors=13; file.declaredScenes=2; file.rejectedScenes=2;
	for (int i=0;i<13;++i) file.problems.push_back({.code="invalid",.message="Problem "+std::to_string(i)});
	catalog.files.push_back(file);

	namespace Catalog=UIBridgeCatalog;
	const auto cards=Catalog::BuildCatalog(false);
	Check(cards.size() == 1 && cards[0]["id"] == "custom.wave", "catalog uses public copied metadata and excludes missing/library entries");
	Check(cards[0]["estSec"] == 6.0f && cards[0]["stages"][0]["loopSec"] == 3.0f, "authored loop durations retain presentation estimates");
	const auto clips=Catalog::BuildCatalog(true);
	Check(clips.size() == 1 && clips[0]["estSec"] == 4.0f && clips[0]["curated"] == true, "library frozen-stage timing is preserved");
	Check(Catalog::BuildWheelData("")["entries"].size() == 1, "wheel defaults preserve case-insensitive tag eligibility");
	OSF::Serialization::WheelPins::customized=true;
	OSF::Serialization::WheelPins::entries={{"LIBRARY.WAVE",0}};
	const auto wheel=Catalog::BuildWheelData("");
	Check(wheel["customized"] == true && wheel["entries"][0]["scene"] == "LIBRARY.WAVE" && wheel["entries"][0]["stage"] == 0, "saved pins retain scene ID and stage identity");
	const auto report=Catalog::BuildFileReport();
	Check(report["files"][0]["problems"].size() == 12 && report["files"][0]["problemCount"] == 13, "presentation bounds diagnostics without changing totals");
	Check(report["totals"]["rejectedFiles"] == 1 && report["totals"]["errors"] == 13, "rejected files with no scenes remain visible");
	Check(Catalog::BuildImportTextReport("broken.osf.json")->find("Problem 12") != std::string::npos, "clipboard report retains all diagnostics");
	Check(!Catalog::BuildImportTextReport("unknown"), "unknown import path cannot read arbitrary files");
	Check(reads == 1, "browser caches one owned metadata snapshot");
	catalog.scenes.front().name="Updated";
	EngineClient::CatalogData::GetSingleton().Invalidate();
	Check(Catalog::BuildCatalog(false)[0]["title"] == "Updated" && reads == 2, "catalog invalidation refreshes copied metadata");
	return OSF::Test::Finish("browser boundary");
}
