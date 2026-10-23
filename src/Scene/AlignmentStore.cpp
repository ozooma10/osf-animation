#include "Scene/AlignmentStore.h"
#include "API/Health.h"
#include "Util/RegistryFiles.h"
#include <fstream>
#include <Windows.h>
#undef ERROR

namespace OSF::Scene::Alignment
{
	namespace
	{
		std::mutex g_lock;
		Table g_table;
		bool g_loadFailed = false;
#ifdef OSF_ANIMATION_SMOKE
		std::filesystem::path g_smokeFile;
#endif
		std::filesystem::path UserFilePath()
		{
#ifdef OSF_ANIMATION_SMOKE
			if (!g_smokeFile.empty()) return g_smokeFile;
#endif
			if (const auto dir = SFSE::log::log_directory()) return dir->parent_path() / "OSF" / "alignment-overrides.json";
			return {};
		}
		void Problem(const char* detail)
		{
			REX::ERROR("[Scene] alignment-overrides.json: {}", detail);
			API::Health::Report("alignment.overrides", "alignment.storage", API::Health::Severity::kWarning, detail);
		}
	}
#ifdef OSF_ANIMATION_SMOKE
	void SetFileForSmoke(const std::filesystem::path& file)
	{
		{ std::lock_guard l{ g_lock }; g_smokeFile = file; g_table.clear(); }
		Load();
	}
#endif
	void Load()
	{
		std::lock_guard l{ g_lock };
		// A rejected document may contain rows absent from memory. Never overwrite it on save.
		g_loadFailed = true;
		try {
			const auto file = UserFilePath();
			if (file.empty()) { Problem("User directory is unavailable; alignment overrides were not loaded."); return; }
			if (!std::filesystem::exists(file)) { g_table.clear(); g_loadFailed = false; API::Health::Clear("alignment.overrides"); return; }
			if (std::filesystem::file_size(file) > Util::kMaxRegistryFileBytes) { Problem("Alignment file exceeds the size limit; previous overrides retained."); return; }
			std::ifstream input(file, std::ios::binary);
			std::size_t skipped = 0;
			const auto next = input ? Parse(nlohmann::json::parse(input, nullptr, false, true), &skipped) : std::nullopt;
			if (!next) { Problem("Invalid or unreadable alignment file; previous overrides retained."); return; }
			g_table = *next;
			g_loadFailed = false;
			if (skipped) Problem("Invalid alignment entries were skipped; valid overrides were loaded.");
			else API::Health::Clear("alignment.overrides");
		} catch (...) { Problem("Alignment file could not be loaded; previous overrides retained."); }
	}
	std::vector<Delta> Read(std::string_view node, const std::vector<std::string>& roles)
	{
		std::lock_guard l{ g_lock };
		std::vector<Delta> result;
		for (const auto& role : RoleKeys(roles)) result.push_back(Find(g_table, node, role));
		return result;
	}
	bool Save(const std::string& node, const std::vector<std::string>& roles, const std::vector<Delta>& values)
	{
		if (node.empty() || roles.size() != values.size() || !std::ranges::all_of(values, Valid)) return false;
		std::lock_guard l{ g_lock };
		if (g_loadFailed) { Problem("Alignment file was not loaded; repair it and reload before saving. Changes remain unsaved."); return false; }
		try {
			auto next = g_table;
			const auto keys = RoleKeys(roles);
			for (std::size_t i = 0; i < keys.size(); ++i) Put(next, node, keys[i], values[i]);
			const auto doc = Serialize(next);
			const auto text = doc.dump(2);
			std::size_t skipped = 0;
			if (text.size() > Util::kMaxRegistryFileBytes || !Parse(doc, &skipped) || skipped) { Problem("Alignment table contains invalid entries or exceeds supported limits; changes remain unsaved."); return false; }
			const auto file = UserFilePath();
			if (file.empty()) { Problem("User directory is unavailable; changes remain unsaved."); return false; }
			std::filesystem::create_directories(file.parent_path());
			auto tmp = file; tmp += ".tmp";
			std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
			output << text;
			output.close();
			if (!output || !::MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				Problem("Alignment file could not be replaced; changes remain unsaved."); return false;
			}
			g_table = std::move(next);
			API::Health::Clear("alignment.overrides");
			return true;
		} catch (...) { Problem("Alignment file could not be saved; changes remain unsaved."); return false; }
	}
}
