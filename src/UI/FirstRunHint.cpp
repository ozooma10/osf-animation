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

		std::mutex g_lock;  // guards the counters + the state file
		bool       g_loaded = false;
		int        g_opens = 0;
		int        g_shows = 0;

		// <Documents>\My Games\Starfield\OSF\first-run.json (next to the clip-duration cache),
		// or empty (no persistence — counters still work for this session).
		std::filesystem::path StatePath()
		{
			try {
				if (auto dir = SFSE::log::log_directory()) {
					// ...\My Games\Starfield\SFSE\Logs -> ...\My Games\Starfield\OSF
					return dir->parent_path().parent_path() / "OSF" / "first-run.json";
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
	}

	void OnPostLoad()
	{
		if (!g_enabled.load(std::memory_order_relaxed)) {
			return;
		}
		if (!API::UIBridgeInstalled()) {
			return;  // no OSF UI -> F10 opens nothing; the hint would point at a dead key
		}
		std::lock_guard lock(g_lock);
		LoadLocked();
		if (g_opens >= kOpensToLearn || g_shows >= kMaxShows) {
			return;
		}
		// The default OSF UI toggle; a player who remapped toggleKey already found the menu.
		HudMessage::Show("OSF — press F10 to open the scene browser");
		++g_shows;
		SaveLocked();
		REX::DEBUG("[FirstRun] hint shown ({} of {} shows used; {} of {} opens seen)",
			g_shows, kMaxShows, g_opens, kOpensToLearn);
	}

	void OnMenuOpened()
	{
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
