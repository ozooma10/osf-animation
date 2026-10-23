#include "Check.h"
#include "Util/KeywordLabel.h"
void RunBrowserClipLibraryTests();
void RunBrowserCatalogJsonTests();
void RunBrowserCatalogJsonBenchmark();
#include "API/UIBridgeCatalog.h"
#include "API/EngineClient.h"
#include "API/BrowserSessionQueue.h"
#include "API/BrowserLaunchSequence.h"
#include "API/BrowserJsonTransport.h"
#include "API/EngineServiceState.h"
#include "API/BrowserForwarder.h"
#include "API/BrowserInput.h"
#include "Serialization/BrowserPreferences.h"
#include "Serialization/WheelPins.h"
#include <atomic>
#include <future>
#include <thread>
#include <fstream>

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
	std::atomic<int> reads{};
	std::atomic<bool> cancelNextPost{};
	std::atomic<int> canceledPosts{};
	std::optional<std::string> catalogTextOverride;
	bool ReadCatalog(OSF::API::TextCallback callback, void* context)
	{
		++reads;
		const auto text=catalogTextOverride.value_or(nlohmann::json(catalog).dump());
		callback(text.c_str(),context);
		return true;
	}
	void RunCatalogRefreshTests()
	{
		using OSF::Test::Check;
		using Queue = OSF::API::EngineClient::CatalogRefreshQueue;
		std::promise<void> entered, release, releaseLatest;
		auto gate = release.get_future();
		auto latestGate = releaseLatest.get_future();
		std::mutex lock;
		std::condition_variable wake;
		int notified{};
		std::atomic<int> calls{};
		const auto owner = std::this_thread::get_id();
		std::atomic<bool> offThread{};
		Queue queue([&]() -> std::unique_ptr<OSF::Catalog::Snapshot> {
			offThread = std::this_thread::get_id() != owner;
			const auto call = ++calls;
			if (call == 1) { entered.set_value(); gate.wait(); }
			if (call == 3) return {};
			if (call == 4) throw std::runtime_error("fixture read failure");
			if (call == 6) latestGate.wait();
			auto snapshot = std::make_unique<OSF::Catalog::Snapshot>();
			snapshot->errors.push_back(std::to_string(call));
			return snapshot;
		}, [&] { std::scoped_lock guard{lock}; ++notified; wake.notify_one(); });
		const auto wait = [&](int count) {
			std::unique_lock guard{lock};
			Check(wake.wait_for(guard, std::chrono::seconds(3), [&] { return notified >= count; }), "catalog worker completes within deadline");
		};
		auto current = std::make_unique<OSF::Catalog::Snapshot>();
		current->errors = {"previous"};
		queue.Request(true);
		Check(entered.get_future().wait_for(std::chrono::seconds(3)) == std::future_status::ready, "catalog load started on worker");
		queue.Request(); queue.Request();
		Check(!queue.Publish(current) && current->errors[0] == "previous", "readers keep last good catalog without waiting for blocked IO");
		release.set_value(); wait(1);
		auto completed = queue.Publish(current);
		Check(offThread && calls == 2 && completed && completed->success && completed->scan && current->errors[0] == "2",
			"overlapping refreshes coalesce, discard stale result, and retain requested scan");
		Check(!queue.Publish(current), "completed catalog publishes once");
		queue.Request(); wait(2); completed = queue.Publish(current);
		Check(completed && !completed->success && current->errors[0] == "2", "failed refresh reports failure without clearing last good catalog");
		queue.Request(); wait(3); completed = queue.Publish(current);
		Check(completed && !completed->success && current->errors[0] == "2", "throwing loader remains retryable");
		queue.Request(true); wait(4);
		queue.Request();
		Check(!queue.Publish(current), "already completed result is rejected after a newer invalidation");
		releaseLatest.set_value();
		wait(5); completed = queue.Publish(current);
		Check(completed && completed->success && completed->scan && current->errors[0] == "6",
			"replacement retains scan intent even when previous ready result was never adopted");
	}

	void RunBrowserLaunchSequenceTests()
	{
		using OSF::Test::Check;
		using Sequence = OSF::API::BrowserLaunchSequence;
		namespace Queue = OSF::Util::NativeMainThreadQueue;
		struct Fixture
		{
			OSF::API::BrowserSessionQueue session{ [](auto task) { Queue::Post(std::move(task)); } };
			Sequence::Requests requests;
			std::vector<std::int32_t> owners, stopped;
			std::vector<int> startedRequests, failedRequests;
			int starts{}, failures{};
			bool acceptStop = true;
			Sequence::Clock::time_point now{};
			Fixture() { session.Visibility(true, [] {}); Queue::Drain(); }
			void Begin(std::shared_ptr<int> pin = {})
			{
				BeginFor({ 10 }, owners, 0, std::move(pin));
			}
			void BeginFor(std::vector<std::uint32_t> cast, std::vector<std::int32_t>& currentOwners,
				int request, std::shared_ptr<int> pin = {})
			{
				Sequence::Begin(requests, std::move(cast), {
					.owners = [&currentOwners] { return currentOwners; },
					.stop = [this](auto owner) {
						const bool alreadyStopping = std::find(stopped.begin(), stopped.end(), owner) != stopped.end();
						stopped.push_back(owner);
						return acceptStop && !alreadyStopping;
					},
					.start = [this, pin, request] { ++starts; startedRequests.push_back(request); },
					.fail = [this, request](const char*) { ++failures; failedRequests.push_back(request); },
					.post = [this](auto task) { session.Post(std::move(task)); },
					.now = [this] { return now; }
				});
			}
		};
		{
			Fixture f; f.owners = { 0, 0 }; f.Begin();
			Check(f.starts == 1 && f.stopped.empty() && !f.failures, "free cast starts immediately");
			Queue::Drain(); Check(f.starts == 1, "immediate launch is not also posted");
		}
		{
			Fixture f; f.owners = { 17, 17, 23 }; f.Begin();
			Check(f.stopped == std::vector<std::int32_t>{ 17, 23 } && !f.starts, "shared owners stop once and accepted stops do not imply release");
			Queue::Post([&] { f.owners = { 0, 0, 23 }; });
			Queue::Drain(); Queue::Drain();
			Check(!f.starts && !f.failures, "replacement waits multiple finite frame drains for the entire cast");
			Queue::Post([&] { f.owners = { 0, 0, 0 }; });
			Queue::Drain(); Check(!f.starts, "release later in the frame does not start from stale ownership");
			Queue::Drain(); Queue::Drain();
			Check(f.starts == 1 && !f.failures, "replacement starts once after actual deferred release");
		}
		{
			Fixture f; f.owners = { 17 }; f.acceptStop = false; f.Begin(); Queue::Drain();
			Check(!f.starts && f.failures == 1, "stop refusal never starts a replacement");
		}
		{
			Fixture f; f.owners = { 17 }; f.Begin(); f.now += std::chrono::seconds(5); Queue::Drain(); Queue::Drain();
			Check(!f.starts && f.failures == 1, "cast release timeout terminates the pending launch");
		}
		{
			Fixture f; f.owners = { 17 }; f.Begin(); f.owners = { 29 }; Queue::Drain(); Queue::Drain();
			Check(!f.starts && f.failures == 1 && f.stopped == std::vector<std::int32_t>{ 17 }, "competing launch is neither stopped nor stolen");
		}
		{
			Fixture f;
			std::vector<std::int32_t> older{ 17, 23 }, newer{ 17, 0 };
			f.BeginFor({ 10, 20 }, older, 1);
			f.BeginFor({ 10, 30 }, newer, 2);
			Check(f.stopped == std::vector<std::int32_t>{ 17, 23 }, "overlapping replacement inherits accepted stops without stopping an ending scene twice");
			older = { 0, 0 }; newer = { 0, 0 };
			Queue::Drain(); Queue::Drain();
			Check(f.startedRequests == std::vector<int>{ 2 } && f.failedRequests.empty(), "only the newest overlapping launch starts and the older request sends no stale reply");
		}
		for (const bool sharedOwner : { false, true }) {
			Fixture f;
			std::vector<std::int32_t> left{ 17 }, right{ sharedOwner ? 17 : 23 };
			f.BeginFor({ 10 }, left, 1);
			f.BeginFor({ 20 }, right, 2);
			Check(f.stopped.size() == (sharedOwner ? 1u : 2u), "disjoint pending launches share already accepted scene stops");
			left = { 0 }; right = { 0 };
			Queue::Drain(); Queue::Drain();
			Check(f.startedRequests == std::vector<int>({ 1, 2 }) && f.failedRequests.empty(), "disjoint pending launches remain independent and both start once");
		}
		{
			Fixture f; f.owners = { 17 };
			f.BeginFor({ 10 }, f.owners, 1);
			f.BeginFor({ 10 }, f.owners, 2);
			f.owners = { 29 };
			Queue::Drain(); Queue::Drain();
			Check(f.startedRequests.empty() && f.failedRequests == std::vector<int>{ 2 } && f.stopped == std::vector<std::int32_t>{ 17 }, "newest overlapping request still refuses an outside owner without reviving the old request");
		}
		for (const bool reset : { false, true }) {
			Fixture f; f.owners = { 17 };
			auto priorPin = std::make_shared<int>(1), pin = std::make_shared<int>(2);
			std::weak_ptr<int> priorWeak = priorPin, weak = pin;
			f.BeginFor({ 10 }, f.owners, 1, priorPin); priorPin.reset();
			f.BeginFor({ 10 }, f.owners, 2, pin); pin.reset();
			Check(!weak.expired(), "pending launch retains its request and reference pins");
			if (reset) f.session.Reset([] {}); else f.session.Visibility(false, [] {});
			f.owners = { 0 }; Queue::Drain(); Queue::Drain();
			Check(!f.starts && !f.failures && weak.expired() && priorWeak.expired(), "hide or world reset releases both superseded and newest request pins despite the weak request group");
		}
	}
}

int main(int argc, char** argv)
{
	using OSF::Test::Check;
	using namespace OSF::API;
	if (argc == 2 && std::string_view(argv[1]) == "--catalog-bench") {
		RunBrowserCatalogJsonBenchmark();
		return OSF::Test::Finish("catalog parse benchmark");
	}
	RunBrowserCatalogJsonTests();
	RunBrowserClipLibraryTests();
	RunCatalogRefreshTests();
	RunBrowserLaunchSequenceTests();
	{
		using Transport = BrowserJsonTransport;
		using json = nlohmann::json;
		struct Event { std::string type, text; };
		std::vector<Event> emitted;
		std::vector<std::function<void()>> posted;
		int errors = 0;
		bool acceptEmit = true, acceptPost = true;
		Transport transport(
			[&](const auto& view, const auto& type, const auto& text) {
				Check(view == "osf.animation/browser", "JSON transfer preserves destination view");
				if (!acceptEmit) return false;
				emitted.push_back({ type, text }); return true;
			},
			[&](std::function<void()> task) { if (!acceptPost) return false; posted.push_back(std::move(task)); return true; },
			[&](const auto&, const auto&, const auto*) { ++errors; });
		const auto frame = [&] {
			auto batch = std::move(posted); posted.clear();
			const auto before = emitted.size();
			for (auto& task : batch) task();
			Check(emitted.size() - before <= Transport::kChunksPerFrame, "JSON transfer stays within the per-frame host queue budget");
		};
		const auto drain = [&] { for (int i = 0; !posted.empty() && i < 1024; ++i) frame(); Check(posted.empty(), "JSON transfer drain terminates"); };
		const auto send = [&](std::string type, std::string text) { return transport.Send("osf.animation/browser", std::move(type), std::move(text)); };
		Check(send("catalog", "{\"scenes\":[]}") && emitted.size() == 1 && emitted.back().text == "{\"scenes\":[]}" && posted.empty(),
			"small JSON documents retain the released event payload shape");
		emitted.clear();
		// A multi-megabyte document containing quotes, backslashes, control escapes, and
		// 4-byte characters crosses both the released 1 MiB limit and UTF-8 boundaries.
		std::string name;
		for (int i = 0; i < 160000; ++i) name += "\"\\\n\xF0\x9F\x8E\xAC";
		const auto large = json{ { "scenes", { { { "title", name } } } } }.dump();
		Check(large.size() > 1024u * 1024u && send("library", large) && emitted.empty(), "large library is queued instead of exceeding OSF UI's 1 MiB limit");
		frame();
		const auto firstId = json::parse(emitted.front().text)["id"];
		Check(send("library", large) && posted.size() == 1, "identical bootstrap retries preserve an active transfer");
		drain();
		std::string decoded;
		for (std::size_t i = 0; i < emitted.size(); ++i) {
			const auto envelope = json::parse(emitted[i].text);
			Check(emitted[i].type == "library" && emitted[i].text.size() < 1024u * 1024u,
				"every large-library chunk retains its event and fits the released host limit");
			Check(envelope["osfAnimationTransfer"] == 1 && envelope["id"] == firstId && envelope["index"] == i &&
				envelope["count"] == emitted.size() && envelope["bytes"] == large.size(), "JSON transfer metadata identifies one ordered document");
			const auto part = envelope["data"].get<std::string>();
			Check(!part.empty() && part.size() <= Transport::kChunkBytes, "UTF-8 chunk obeys the raw byte bound");
			decoded += part;
		}
		Check(decoded == large && json::parse(decoded)["scenes"][0]["title"] == name, "large JSON and multibyte names round-trip exactly");
		emitted.clear();
		Check(send("catalog", large) && send("library", large), "independent large event streams may overlap");
		frame();
		Check(emitted.size() == 4 && emitted[0].type == "catalog" && emitted[1].type == "library" &&
			emitted[2].type == "catalog" && emitted[3].type == "library", "overlapping JSON streams share one fair frame budget");
		transport.Clear(); emitted.clear();
		Check(send("library", large), "new document can start immediately after hide/reset");
		frame();
		Check(emitted.size() == 4, "stale scheduled callbacks cannot advance a replacement session");
		Check(send("library", "{\"scenes\":[]}"), "small snapshot can supersede an active large document");
		const auto replacedSize = emitted.size(); drain();
		Check(emitted.size() == replacedSize && emitted.back().text == "{\"scenes\":[]}", "replacement cancels remaining old chunks");
		emitted.clear();
		Check(send("library", large), "large replacement fixture starts"); frame();
		const auto replacedId = json::parse(emitted.front().text)["id"];
		Check(send("library", large + " "), "changed snapshot supersedes an active transfer");
		emitted.clear(); drain();
		Check(!emitted.empty() && json::parse(emitted.front().text)["index"] == 0 &&
			json::parse(emitted.front().text)["id"] != replacedId, "replacement JSON receives a new transfer identity");
		emitted.clear();
		acceptEmit = false;
		Check(send("library", large), "emit failure fixture queues"); drain();
		Check(errors == 1 && emitted.empty(), "host rejection aborts a transfer and reports an error");
		acceptEmit = true; acceptPost = false;
		Check(!send("library", large) && errors == 2, "frame queue rejection fails explicitly and clears retained data");
		acceptPost = true;
		Check(send("library", large), "transport recovers after a refused frame queue"); transport.Clear(); drain();
		Check(!send("library", std::string(Transport::kMaxBytes + 1, ' ')) && posted.empty(), "oversized logical documents are refused before scheduling");
		for (std::size_t i = 0; i < Transport::kMaxTransfers; ++i) Check(send("stream" + std::to_string(i), large), "bounded concurrent stream accepted");
		Check(!send("excess", large), "concurrent transfer count is bounded"); transport.Clear(); drain();
		const std::string maximum(Transport::kMaxBytes, ' ');
		Check(send("first", maximum) && send("second", maximum) && !send("third", large), "total queued transfer memory is bounded");
		transport.Clear(); drain();
	}
	{
		Check(OSF::Util::AnimationKeywordLabel("AnimFurnChairScrappy") == "Chair Scrappy",
			"animation keyword prefix and CamelCase are humanized");
		Check(OSF::Util::AnimationKeywordLabel("AnimHVACUnit") == "HVAC Unit",
			"animation keyword acronym boundary is preserved");
	}
	namespace Queue=OSF::Util::NativeMainThreadQueue;
	{
		OSF::API::BrowserSessionQueue session([](std::function<void()> task) { OSF::Util::NativeMainThreadQueue::Post(std::move(task)); });
		std::vector<int> events;
		session.Post([&] { events.push_back(99); }); // hidden mutations are refused
		session.Post([&] { events.push_back(1); }, false); // document bootstrap
		session.Visibility(true, [&] { events.push_back(2); });
		OSF::Util::NativeMainThreadQueue::Drain();
		Check(events == std::vector<int>{ 1, 2 }, "document init may precede native show");
		session.Post([&] { events.push_back(99); });
		session.Visibility(false, [&] { events.push_back(3); });
		session.Visibility(false, [&] { events.push_back(99); });
		session.Visibility(true, [&] { events.push_back(4); });
		session.Post([&] { events.push_back(5); });
		OSF::Util::NativeMainThreadQueue::Drain();
		Check(events == std::vector<int>{ 1, 2, 3, 4, 5 }, "close drops stale commands and cleans once before reopen");
		session.Post([&] { events.push_back(99); }, false);
		session.Reset([&] { events.push_back(6); });
		OSF::Util::NativeMainThreadQueue::Drain();
		Check(events.back() == 6 && events.size() == 6, "world replacement invalidates pending work including opens");
		session.Visibility(true, [&] { events.push_back(99); });
		session.Visibility(false, [&] { events.push_back(7); });
		OSF::Util::NativeMainThreadQueue::Drain();
		Check(events.back() == 7 && events.size() == 7, "canceled show never engages camera but hide still cleans up");
	}

	Check(RequestSceneAPI() == nullptr && RequestEngineAPI() == nullptr, "absent engine discovery is normal");
	Check(!OpenBrowser(), "Papyrus-compatible forwarding is false with no addon DLL");
	Check(!EngineClient::Connect(nullptr) && !EngineClient::Post([]{}), "absent provider refuses posts");
	OSFEngineAPI api{};
	api.size=sizeof(api); api.version=kOSFEngineAPIVersion+1;
	Check(EngineClient::Connect(&api), "newer compatible minor accepted");
	api.version=0x00050000;
	Check(!EngineClient::Connect(&api), "different services major refused");
	for (const auto version : { 0x00030000u, 0x0003000Cu, 0x0003000Du, 0x0003000Eu, 0x00030011u }) {
		api.version = version;
		Check(!EngineClient::Connect(&api), "all pre-v2 services tables are refused before use");
		Check(!EngineRequestSupported(version, kOSFEngineAPIVersion), "v2 host refuses every legacy services request");
	}
	Check(EngineRequestSupported(kOSFEngineAPIVersion, kOSFEngineAPIVersion) &&
		EngineRequestSupported(kOSFEngineAPIVersion, kOSFEngineAPIVersion + 1) &&
		!EngineRequestSupported(kOSFEngineAPIVersion + 1, kOSFEngineAPIVersion),
		"services minor compatibility starts at the complete 4.0 baseline");
	Check(SceneRequestSupported(kOSFSceneAPIVersion, kOSFSceneAPIVersion) &&
		SceneRequestSupported(kOSFSceneAPIVersion, kOSFSceneAPIVersion + 1) &&
		!SceneRequestSupported(kOSFSceneAPIVersion + 1, kOSFSceneAPIVersion) &&
		!SceneRequestSupported(0x00010007, kOSFSceneAPIVersion) &&
		!SceneRequestSupported(kOSFSceneAPIVersion, 0x00010007),
		"scene API minor compatibility starts at 2.0 and rejects v1 in both directions");
	api.version=kOSFEngineAPIVersion; api.size=8;
	Check(!EngineClient::Connect(&api), "truncated table refused before use");
	api.size=sizeof(api); api.ReadCatalog=ReadCatalog;
	api.Post=[](FrameCallback callback, void* context) {
		const bool cancel = cancelNextPost.exchange(false);
		OSF::Util::NativeMainThreadQueue::Post([=] {
			callback(context, !cancel);
			if (cancel) ++canceledPosts;
		}); return true;
	};
	api.ReadClipDurations=[](TextCallback callback,void* context) { callback("{}",context); return true; };
	Check(EngineClient::Connect(&api), "complete public services table accepted");
	{
		static std::uint32_t routing{};
		static float dx{}, dy{}, wheel{};
		api.SetInputRouting=[](std::uint32_t flags) { routing=flags; };
		api.InjectOrbitDelta=[](float x,float y,float w) { dx=x; dy=y; wheel=w; };
		BrowserInput::SetVisible(api,true);
		Check(routing == (SuppressRawMouse | ConsumeThumbsticks | ConsumeGamepadAfterDispatch | AllowNativeFreeCamera),
			"browser requests its input routing explicitly");
		BrowserInput::Orbit(api,10,-20,2);
		Check(dx == 6 && dy == -12 && wheel == 2, "addon converts logical pointer pixels, preserving wheel units");
		BrowserInput::SetVisible(api,false);
		Check(routing == 0, "browser hide releases its input policy");
	}

	EngineServiceState engine;
	{
		using json=nlohmann::json;
		using Preferences=OSF::Serialization::BrowserPreferences;
		const auto root=std::filesystem::temp_directory_path() / ("osf-browser-preferences-" +
			std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directories(root);
		const auto file=root / "browser-preferences.json";
		Preferences preferences(file);
		std::string error;
		Check(preferences.Load(error) && preferences.Values().size() == 10 &&
			preferences.Values()["browser.afterLaunch"] == "stay" && preferences.Values()["browser.authorDetails"] == false &&
			preferences.Values()["browser.openTo"] == "last" && preferences.Values()["browser.actorLabels"] == true &&
			!std::filesystem::exists(file), "missing preference file uses defaults without writing a file");
		Check(preferences.Set("browser.authorDetails",true,error) && preferences.Set("launch.camera","freefly",error),
			"browser preferences create and replace the saved file");
		Preferences reloaded(file);
		Check(reloaded.Load(error) && reloaded.Values()["browser.authorDetails"] == true && reloaded.Values()["launch.camera"] == "freefly",
			"saved browser choices survive restart");
		const json rejected={{"launch.speed","2"},{"browser.rememberBrowsing",false},
			{"logLevel","trace"},{"launch.camera","invalid"},{"browser.actorLabels","false"},{"browser.openTo",3}};
		for (const auto& [key,value] : rejected.items())
			Check(!preferences.Set(key,value,error), "unknown, retired and mistyped preferences are rejected");
		const auto before=preferences.Values();
		std::filesystem::create_directory(file.string()+".tmp");
		Check(!preferences.Set("browser.actorLabels",false,error) && preferences.Values() == before,
			"failed write does not publish unsaved preferences");
		std::filesystem::remove(file.string()+".tmp");
		Preferences afterFailure(file);
		Check(afterFailure.Load(error) && afterFailure.Values() == before, "failed write preserves the previous disk snapshot");
		// Malformed or future-version storage is preserved and cannot be overwritten by a UI edit.
		for (const auto* contents : { "broken", "{\"formatVersion\":2,\"values\":{}}" }) {
			std::ofstream(root / "invalid.json") << contents;
			Preferences invalid(root / "invalid.json");
			Check(!invalid.Load(error) && !invalid.Set("browser.actorLabels",false,error), "unreadable preferences refuse edits");
			std::ifstream input(root / "invalid.json");
			Check(std::string(std::istreambuf_iterator<char>(input),{}) == contents, "unreadable file remains unchanged");
		}
		Preferences smoke(std::filesystem::path{});
		Check(smoke.Load(error) && smoke.Set("browser.actorLabels",false,error), "smoke preferences work without player storage");
		std::filesystem::remove_all(root); // Unique temporary fixture created above.
	}
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

	OSF::Catalog::Node emote;
	emote.id="custom.wave"; emote.name="Wave"; emote.tags={"Player.Emote.Wave"}; emote.actorCount=1;
	emote.roles.push_back({"actor","any"}); emote.placement="followActor";
	emote.sourceKind="authoredScene";
	emote.stages.push_back({.index=0,.clipCount=1,.loopCount=2,.name="Wave loop",.loopMode="count",.tags={"wave"},.loopSec=3.0f,.resolved=true});
	catalog.nodes.push_back(emote);
	auto library=emote; library.id="library.wave"; library.library=true; library.curatedClip=true; library.sourceKind="curatedAnimation";
	library.name="Library wave"; library.stages.front().hold=0.5f; library.stages.front().timerSec=4.0f;
	catalog.nodes.push_back(library);
	auto missing=emote; missing.id="missing.wave"; missing.clipsAvailable=false; catalog.nodes.push_back(missing);
	OSF::Catalog::File file; file.path="broken.osf.json"; file.file=file.path; file.errors=13; file.declaredNodes=2; file.rejectedNodes=2;
	for (int i=0;i<13;++i) file.problems.push_back({.code="invalid",.message="Problem "+std::to_string(i)});
	catalog.files.push_back(file);
	int catalogUpdates{};
	bool lastCatalogSuccess{};
	auto& catalogData = EngineClient::CatalogData::GetSingleton();
	catalogData.Initialize([&](bool ok) { lastCatalogSuccess = ok; ++catalogUpdates; });
	const auto awaitCatalog = [&](int updates) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (catalogUpdates < updates && std::chrono::steady_clock::now() < deadline) {
			OSF::Util::NativeMainThreadQueue::Drain(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		Check(catalogUpdates == updates, "catalog completion returns to the frame queue");
	};
	catalogData.Invalidate(); awaitCatalog(1);
	Check(lastCatalogSuccess, "initial metadata refresh succeeded");

	namespace Catalog=UIBridgeCatalog;
	const auto cards=Catalog::BuildCatalog(false);
	Check(cards.size() == 1 && cards[0]["id"] == "custom.wave", "catalog uses public copied metadata and excludes missing/library entries");
	Check(cards[0]["estSec"] == 6.0f && cards[0]["stages"][0]["loopSec"] == 3.0f, "authored loop durations retain presentation estimates");
	Check(cards[0]["sourceKind"] == "authoredScene" && cards[0]["placement"] == "followActor" &&
		cards[0]["hideApparel"] == emote.hideApparel && cards[0]["playerInputLock"] == emote.playerInputLock &&
		!cards[0].contains("stripActors") && !cards[0].contains("lockPlayer") && !cards[0].contains("inPlace") && !cards[0].contains("curated"),
		"browser catalog publishes canonical policy and source fields only");
	const auto clips=Catalog::BuildCatalog(true);
	Check(clips.size() == 1 && clips[0]["estSec"] == 4.0f && clips[0]["sourceKind"] == "curatedAnimation" && !clips[0].contains("curated") && !clips[0].contains("inPlace") && !clips[0].contains("lockPlayer") && !clips[0].contains("stripActors"), "library frozen-stage timing is preserved");
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
	catalog.nodes.front().name="Updated";
	EngineClient::CatalogData::GetSingleton().Invalidate();
	awaitCatalog(2);
	Check(Catalog::BuildCatalog(false)[0]["title"] == "Updated" && reads == 2, "catalog invalidation refreshes copied metadata");
	catalogData.InvalidateDurations();
	Check(Catalog::BuildCatalog(false)[0]["title"] == "Updated" && reads == 2, "duration changes do not rebuild or reparse static metadata");
	catalog.nodes.front().name="After reset";
	cancelNextPost = true; catalogData.Invalidate();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!canceledPosts && std::chrono::steady_clock::now() < deadline) {
		OSF::Util::NativeMainThreadQueue::Drain(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Check(canceledPosts == 1 && catalogUpdates == 2 && Catalog::BuildCatalog(false)[0]["title"] == "Updated",
		"canceled frame completion does not publish from getters or clear previous metadata");
	catalogData.PublishPending();
	Check(catalogUpdates == 3 && Catalog::BuildCatalog(false)[0]["title"] == "After reset" && reads == 3,
		"world-reset frame callback resumes retained metadata without another read");
	catalogTextOverride = "{\"schema\":3,\"scenes\":[{\"id\":\"partial\"}]}";
	catalogData.Invalidate(); awaitCatalog(4);
	Check(!lastCatalogSuccess && Catalog::BuildCatalog(false)[0]["title"] == "After reset" && reads == 4,
		"malformed catalog reports failure and retains the last good snapshot through the real Browser worker");
	catalogTextOverride.reset(); catalog.nodes.front().name = "Recovered";
	catalogData.Invalidate(); awaitCatalog(5);
	Check(lastCatalogSuccess && Catalog::BuildCatalog(false)[0]["title"] == "Recovered" && reads == 5,
		"catalog parsing recovers on the next request after a malformed response");
	return OSF::Test::Finish("browser boundary");
}
