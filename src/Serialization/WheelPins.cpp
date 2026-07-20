#include "Serialization/WheelPins.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

#include <nlohmann/json.hpp>

namespace OSF::Serialization::WheelPins
{
	namespace
	{
		using json = nlohmann::json;

		std::mutex               g_lock;  // guards the pin list + the state file
		bool                     g_loaded = false;
		bool                     g_customized = false;
		std::vector<Entry>       g_entries;  // wheel order: front = first slice
		constexpr std::size_t    kMaxEntries = 12;

		// <Documents>\My Games\Starfield\SFSE\OSF\wheel-pins.json,
		// or empty (no persistence — pins still work for this session).
		std::filesystem::path StatePath()
		{
			try {
				if (auto dir = SFSE::log::log_directory()) {
					// ...\My Games\Starfield\SFSE\Logs -> ...\My Games\Starfield\SFSE\OSF
					return dir->parent_path() / "OSF" / "wheel-pins.json";
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
			// The file itself means the user customized the wheel. A malformed file is
			// ignored, which falls back to the installed defaults.
			const json doc = json::parse(in, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
			if (!doc.is_array()) {
				return;
			}
			g_customized = true;
			for (const auto& v : doc) {
				if (!v.is_object()) {
					continue;
				}
				const auto sit = v.find("scene");
				if (sit == v.end() || !sit->is_string()) {
					continue;
				}
				Entry entry;
				entry.scene = sit->get<std::string>();
				if (const auto stit = v.find("stage"); stit != v.end()) {
					if (!stit->is_number_integer()) {
						continue;
					}
					entry.stage = stit->get<std::int32_t>();
				}
				if (!entry.scene.empty() && entry.stage >= -1 && g_entries.size() < kMaxEntries &&
					std::find(g_entries.begin(), g_entries.end(), entry) == g_entries.end()) {
					g_entries.push_back(std::move(entry));
				}
			}
		}

		void SaveLocked()
		{
			const auto file = StatePath();
			if (file.empty()) {
				return;
			}
			json doc = json::array();
			for (const auto& entry : g_entries) {
				json value = { { "scene", entry.scene } };
				if (entry.stage >= 0) {
					value["stage"] = entry.stage;
				}
				doc.push_back(std::move(value));
			}

			std::error_code ec;
			std::filesystem::create_directories(file.parent_path(), ec);
			const auto tmp = file.parent_path() / (file.filename().string() + ".tmp");
			{
				std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
				if (!out) {
					REX::WARN("[UI] cannot write {} — wheel customization won't persist this session", tmp.string());
					return;
				}
				out << doc.dump(1, '\t');
			}
			// std::filesystem::rename does not replace an existing destination on Windows.
			// MoveFileEx gives the temp-write its intended atomic replace semantics.
			if (!::MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				REX::WARN("[UI] cannot replace {} (Win32 error {})", file.string(), ::GetLastError());
			}
		}
	}

	bool Customized()
	{
		std::lock_guard lock(g_lock);
		LoadLocked();
		return g_customized;
	}

	std::vector<Entry> Entries()
	{
		std::lock_guard lock(g_lock);
		LoadLocked();
		return g_customized ? g_entries : std::vector<Entry>{};
	}

	bool SetEntries(std::span<const Entry> a_entries)
	{
		std::vector<Entry> next;
		next.reserve(std::min(a_entries.size(), kMaxEntries));
		for (const auto& entry : a_entries) {
			if (!entry.scene.empty() && entry.stage >= -1 && next.size() < kMaxEntries &&
				std::find(next.begin(), next.end(), entry) == next.end()) {
				next.push_back(entry);
			}
		}
		std::lock_guard lock(g_lock);
		LoadLocked();
		if (g_customized && g_entries == next) {
			return false;
		}
		g_customized = true;
		g_entries = std::move(next);
		SaveLocked();
		return true;
	}

	bool Reset()
	{
		std::lock_guard lock(g_lock);
		LoadLocked();
		bool changed = g_customized || !g_entries.empty();
		g_customized = false;
		g_entries.clear();

		const auto file = StatePath();
		if (!file.empty()) {
			std::error_code ec;
			changed = std::filesystem::remove(file, ec) || changed;
			if (ec) {
				REX::WARN("[UI] cannot remove {} ({})", file.string(), ec.message());
			}

			// Best-effort cleanup if a previous atomic save was interrupted.
			const auto tmp = file.parent_path() / (file.filename().string() + ".tmp");
			ec.clear();
			std::filesystem::remove(tmp, ec);
		}
		return changed;
	}
}
