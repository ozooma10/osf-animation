#include "Animation/AfxStubber.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace OSF::Animation
{
	namespace
	{
		bool ExtIs(const std::filesystem::path& a_p, std::string_view a_ext)
		{
			const auto e = a_p.extension().string();
			if (e.size() != a_ext.size()) {
				return false;
			}
			for (std::size_t i = 0; i < e.size(); ++i) {
				if (std::tolower(static_cast<unsigned char>(e[i])) !=
					std::tolower(static_cast<unsigned char>(a_ext[i]))) {
					return false;
				}
			}
			return true;
		}

		void WriteStub(const std::filesystem::path& a_afx, const std::string& a_afName)
		{
			std::ofstream out{ a_afx, std::ios::binary | std::ios::trunc };
			if (!out) {
				REX::WARN("AfxStubber: could not write {}", a_afx.string());
				return;
			}
			// The engine loads the .af by its GNAM path, so the <filename> here is cosmetic; a generic
			// stub is sufficient (proven in-game 2026-06-23).
			out << "<root>\n"
				   "\t<is_state>1</is_state>\n"
				   "\t<tag>OSF</tag>\n"
				   "\t<filename>"
				<< a_afName
				<< "</filename>\n"
				   "\t<prefix>OSF</prefix>\n"
				   "\t<flags>\n"
				   "\t\t<flag>Animation Driven</flag>\n"
				   "\t</flags>\n"
				   "</root>\n";
		}
	}

	void EnsureAfxStubs()
	{
		// Matches the Data-path convention used across OSF (current_path()/"Data"). Only LOOSE files
		// appear on the filesystem (archived .af do not), so this scans just the custom .af an author
		// shipped, not the whole vanilla animation tree.
		const auto root = std::filesystem::current_path() / "Data" / "meshes" / "actors";

		std::error_code ec;
		if (!std::filesystem::exists(root, ec) || ec) {
			REX::INFO("AfxStubber: '{}' not present; no stubs written", root.string());
			return;
		}

		std::size_t written = 0;
		std::size_t scanned = 0;
		for (auto it = std::filesystem::recursive_directory_iterator{
				 root, std::filesystem::directory_options::skip_permission_denied, ec };
			 it != std::filesystem::recursive_directory_iterator{}; it.increment(ec)) {
			if (ec) {
				ec.clear();
				continue;
			}
			const auto& p = it->path();
			if (!it->is_regular_file(ec) || ec) {
				ec.clear();
				continue;
			}
			if (!ExtIs(p, ".af")) {
				continue;
			}
			++scanned;
			auto afx = p;
			afx.replace_extension(".afx");
			if (std::filesystem::exists(afx, ec) && !ec) {
				continue;  // author shipped one — respect it
			}
			ec.clear();
			WriteStub(afx, p.filename().string());
			++written;
		}

		REX::INFO("AfxStubber: scanned {} loose .af, wrote {} stub .afx under {}", scanned, written, root.string());
	}
}
