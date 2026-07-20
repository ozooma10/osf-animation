#include "UI/FirstRunHint.h"

#include "API/UIBridge.h"
#include "UI/HudMessage.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>

#include <nlohmann/json.hpp>

namespace OSF::UI::FirstRunHint
{
	namespace
	{
		using json = nlohmann::json;

		// The nudge retires once the browser has been opened this many times...
		constexpr int kOpensToLearn = 3;
		// ...or after this many loads have shown it. A player using OSF purely as a backend
		// for another mod may never open the browser — don't nag them forever.
		constexpr int kMaxShows = 10;

		std::atomic_bool g_enabled{ true };
		std::atomic_bool g_pending{ false };  // a hint is armed, waiting for the HUD to come up

		std::mutex g_lock;  // guards the counters + the state file
		bool       g_loaded = false;
		int        g_opens = 0;
		int        g_shows = 0;

		// <Documents>\My Games\Starfield\SFSE\OSF\first-run.json (next to the clip-duration cache),
		// or empty (no persistence — counters still work for this session).
		std::filesystem::path StatePath()
		{
			try {
				if (auto dir = SFSE::log::log_directory()) {
					// ...\My Games\Starfield\SFSE\Logs -> ...\My Games\Starfield\SFSE\OSF
					return dir->parent_path() / "OSF" / "first-run.json";
				}
			} catch (...) {}
			return {};
		}

		void LoadLocked()
		{
			if (g_loaded) {
				return;
			}
			g_loaded = true;

			const auto file = StatePath();
			if (file.empty()) {
				return;
			}
			std::ifstream in(file, std::ios::binary);
			if (!in) {
				return;  // first run
			}
			// Tolerate a corrupt/hand-edited file: unreadable fields count as 0, never thrown.
			const json doc = json::parse(in, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
			if (doc.is_object()) {
				g_opens = doc.value("menuOpens", 0);
				g_shows = doc.value("hintShows", 0);
			}
		}

		void SaveLocked()
		{
			const auto file = StatePath();
			if (file.empty()) {
				return;
			}
			const json doc = { { "version", 1 }, { "menuOpens", g_opens }, { "hintShows", g_shows } };

			std::error_code ec;
			std::filesystem::create_directories(file.parent_path(), ec);
			const auto tmp = file.parent_path() / (file.filename().string() + ".tmp");
			{
				std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
				if (!out) {
					REX::WARN("[FirstRun] cannot write {} — hint state won't persist this session", tmp.string());
					return;
				}
				out << doc.dump(1, '\t');
			}
			std::filesystem::rename(tmp, file, ec);
			if (ec) {
				REX::WARN("[FirstRun] cannot replace {} ({})", file.string(), ec.message());
			}
		}

		// Fire the popup if the HUD is up and the gates still pass. Disarms on delivery or
		// retirement; stays armed (the menu-open sink below retries) while the HUD is down.
		// Showing straight from the load-game sink drops the message silently — the event
		// source exists but the HUD isn't taking notifications yet — and still burned a
		// show, so the budget emptied invisibly. Only a delivered message costs a show.
		void ShowNow()
		{
			if (!g_pending.load(std::memory_order_relaxed)) {
				return;
			}
			std::lock_guard lock(g_lock);
			// Re-check the gates: the player may have opened the browser (or turned the hint
			// off) during the wait, which retires the nudge before it ever appears.
			if (!g_enabled.load(std::memory_order_relaxed) || g_opens >= kOpensToLearn || g_shows >= kMaxShows) {
				REX::INFO("[FirstRun] retired while armed (enabled={}, opens={}, shows={})",
					g_enabled.load(std::memory_order_relaxed), g_opens, g_shows);
				g_pending.store(false, std::memory_order_relaxed);
				return;
			}
			auto* ui = RE::UI::GetSingleton();
			if (!ui || !ui->IsMenuOpen("HUDMenu")) {
				REX::INFO("[FirstRun] HUD not open yet — staying armed for the HUDMenu-open event");
				return;  // still on the load screen — stay armed, the HUDMenu-open event retries
			}
			// The default OSF UI toggle; a player who remapped toggleKey already found the menu.
			HudMessage::Show("OSF — press F10 to open the scene browser");
			++g_shows;
			SaveLocked();
			REX::INFO("[FirstRun] hint shown ({} of {} shows used; {} of {} opens seen)",
				g_shows, kMaxShows, g_opens, kOpensToLearn);
			g_pending.store(false, std::memory_order_relaxed);
		}

		// Waiting for the HUD can't be an AddTask frame-poll: SFSE drains its task queue
		// until empty, so a task that re-queues itself runs again in the SAME drain and a
		// frame budget burns out in one real frame — on the load black screen, before the
		// HUD exists. Listen for the engine's own HUDMenu-open event instead.
		class HudOpenSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static HudOpenSink* GetSingleton()
			{
				static HudOpenSink instance;
				return &instance;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event,
				RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				// Engine menu names are interned case-insensitively — match them the same way.
				if (g_pending.load(std::memory_order_relaxed) && a_event.opening &&
					a_event.menuName.c_str() && _stricmp(a_event.menuName.c_str(), "HUDMenu") == 0) {
					REX::INFO("[FirstRun] HUDMenu opened — scheduling the hint");
					// Ride a task so the fresh HUD gets a beat to start taking notifications.
					SFSE::GetTaskInterface()->AddTask([]() { ShowNow(); });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void OnPostLoad()
	{
		if (!g_enabled.load(std::memory_order_relaxed)) {
			REX::INFO("[FirstRun] OnPostLoad: hint disabled via settings — not arming");
			return;
		}
		if (!API::UIBridgeInstalled()) {
			REX::INFO("[FirstRun] OnPostLoad: OSF UI absent — not arming");
			return;  // no OSF UI -> F10 opens nothing; the hint would point at a dead key
		}
		{
			std::lock_guard lock(g_lock);
			LoadLocked();
			if (g_opens >= kOpensToLearn || g_shows >= kMaxShows) {
				REX::INFO("[FirstRun] OnPostLoad: retired (opens={}, shows={})", g_opens, g_shows);
				return;
			}
		}
		// A quickload while a hint is armed just leaves the one arm in place.
		if (g_pending.exchange(true, std::memory_order_relaxed)) {
			return;
		}
		REX::INFO("[FirstRun] OnPostLoad: armed — waiting for the HUD");
		static std::once_flag sinkOnce;
		std::call_once(sinkOnce, []() {
			// UI exists well before any save can finish loading.
			RE::UI::GetSingleton()->RegisterSink<RE::MenuOpenCloseEvent>(HudOpenSink::GetSingleton());
		});
		// The HUD may already be up (load from an in-game menu) and then no open event
		// comes — try once now; if it's still down the sink takes over.
		SFSE::GetTaskInterface()->AddTask([]() { ShowNow(); });
	}

	void OnMenuOpened(bool a_browserMode)
	{
		// Only the browser teaches the browser hotkey. The wheel is the same view on a
		// separately-bound key (default B), so counting its opens retired the hint for
		// anyone who used the wheel three times — before it had ever been shown.
		if (!a_browserMode) {
			return;
		}
		std::lock_guard lock(g_lock);
		LoadLocked();
		if (g_opens >= kOpensToLearn) {
			return;  // already learned — don't rewrite the file on every open forever
		}
		++g_opens;
		SaveLocked();
		REX::DEBUG("[FirstRun] scene browser opened ({} of {}) — hint {}",
			g_opens, kOpensToLearn, g_opens >= kOpensToLearn ? "retired" : "still armed");
	}

	void SetEnabled(bool a_on)
	{
		g_enabled.store(a_on, std::memory_order_relaxed);
		REX::DEBUG("[FirstRun] hint {}", a_on ? "enabled" : "disabled");
	}
}
