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
		std::vector<std::string> g_entries;  // wheel order: front = first slice
		constexpr std::size_t    kMaxEntries = 12;

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
			// The file itself means the user customized the wheel. A malformed file is
			// ignored, which falls back to the installed defaults.
			const json doc = json::parse(in, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
			if (!doc.is_array()) {
				return;
			}
			g_customized = true;
			for (const auto& v : doc) {
				if (!v.is_string()) {
					continue;
				}
				auto id = v.get<std::string>();
				if (!id.empty() && g_entries.size() < kMaxEntries &&
					std::find(g_entries.begin(), g_entries.end(), id) == g_entries.end()) {
					g_entries.push_back(std::move(id));
				}
			}
		}

		void SaveLocked()
		{
			const auto file = StatePath();
			if (file.empty()) {
				return;
			}
			const json doc = g_entries;

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

	int Order(std::string_view a_sceneId)
	{
		std::lock_guard lock(g_lock);
		LoadLocked();
		if (!g_customized) {
			return 0;
		}
		for (std::size_t i = 0; i < g_entries.size(); ++i) {
			if (g_entries[i] == a_sceneId) {
				return static_cast<int>(i) + 1;
			}
		}
		return 0;
	}

	bool SetEntries(std::span<const std::string> a_sceneIds)
	{
		std::vector<std::string> next;
		next.reserve(std::min(a_sceneIds.size(), kMaxEntries));
		for (const auto& id : a_sceneIds) {
			if (!id.empty() && next.size() < kMaxEntries && std::find(next.begin(), next.end(), id) == next.end()) {
				next.push_back(id);
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
