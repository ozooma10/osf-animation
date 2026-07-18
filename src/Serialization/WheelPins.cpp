#include "Serialization/WheelPins.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace OSF::Serialization::WheelPins
{
	namespace
	{
		using json = nlohmann::json;

		std::mutex               g_lock;  // guards the pin list + the state file
		bool                     g_loaded = false;
		std::vector<std::string> g_pins;  // wheel order: front = first slice

		// <Documents>\My Games\Starfield\OSF\wheel-pins.json (next to first-run.json),
		// or empty (no persistence — pins still work for this session).
		std::filesystem::path StatePath()
		{
			try {
				if (auto dir = SFSE::log::log_directory()) {
					// ...\My Games\Starfield\SFSE\Logs -> ...\My Games\Starfield\OSF
					return dir->parent_path().parent_path() / "OSF" / "wheel-pins.json";
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
				return;  // nothing pinned yet
			}
			// Tolerate a corrupt/hand-edited file: keep the readable pins, drop the rest.
			const json doc = json::parse(in, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
			if (!doc.is_object()) {
				return;
			}
			const auto it = doc.find("pins");
			if (it == doc.end() || !it->is_array()) {
				return;
			}
			for (const auto& v : *it) {
				if (!v.is_string()) {
					continue;
				}
				auto id = v.get<std::string>();
				if (!id.empty() && std::find(g_pins.begin(), g_pins.end(), id) == g_pins.end()) {
					g_pins.push_back(std::move(id));
				}
			}
		}

		void SaveLocked()
		{
			const auto file = StatePath();
			if (file.empty()) {
				return;
			}
			const json doc = { { "version", 1 }, { "pins", g_pins } };

			std::error_code ec;
			std::filesystem::create_directories(file.parent_path(), ec);
			const auto tmp = file.parent_path() / (file.filename().string() + ".tmp");
			{
				std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
				if (!out) {
					REX::WARN("[WheelPins] cannot write {} — wheel pins won't persist this session", tmp.string());
					return;
				}
				out << doc.dump(1, '\t');
			}
			std::filesystem::rename(tmp, file, ec);
			if (ec) {
				REX::WARN("[WheelPins] cannot replace {} ({})", file.string(), ec.message());
			}
		}
	}

	int Order(std::string_view a_sceneId)
	{
		std::lock_guard lock(g_lock);
		LoadLocked();
		for (std::size_t i = 0; i < g_pins.size(); ++i) {
			if (g_pins[i] == a_sceneId) {
				return static_cast<int>(i) + 1;
			}
		}
		return 0;
	}

	bool Set(std::string_view a_sceneId, bool a_pinned)
	{
		if (a_sceneId.empty()) {
			return false;
		}
		std::lock_guard lock(g_lock);
		LoadLocked();
		const auto it = std::find(g_pins.begin(), g_pins.end(), a_sceneId);
		if (a_pinned) {
			if (it != g_pins.end()) {
				return false;  // already pinned — keep its slot
			}
			g_pins.emplace_back(a_sceneId);
		} else {
			if (it == g_pins.end()) {
				return false;
			}
			g_pins.erase(it);  // later pins shift up
		}
		SaveLocked();
		return true;
	}
}
