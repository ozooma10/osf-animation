#pragma once

#include "Registry/ContentRegistry.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace OSF::Registry::ContentRegistryClips
{
	struct ClipLibraryRegistration
	{
		std::string              name;
		std::string              folder;
		std::vector<std::string> tags;
		StageClip                clip;
		std::filesystem::path    sourceFile;
		std::string              pack;
	};

	struct PendingImportProblem
	{
		std::filesystem::path owner;
		bool                  warning = false;
		std::string           code;
		std::string           hint;
		std::string           node;
		std::string           role;
		std::string           clip;
	};

	// Plain-text lines stay stable for Papyrus; `records` is the structured twin for the browser Imports panel.
	struct ProblemSink
	{
		std::vector<std::string>&          lines;
		std::vector<PendingImportProblem>& records;

		void Push(std::string a_line, const std::filesystem::path& a_owner,
			std::string a_code, std::string a_hint = {}, std::string a_node = {}, std::string a_role = {}, std::string a_clip = {});
	};

	using ClipInstalledCache = std::unordered_map<std::string, bool>;

	bool ClipSpecInstalled(const std::string& a_spec);
	void SweepClipAvailability(std::unordered_map<std::string, NodeDef>& a_scenes,
		ProblemSink& a_problems, ClipInstalledCache& a_cache);
	// a_cache is the load's installed-clip cache; every probed entry lands there for the availability pass.
	std::size_t AddRegisteredClipEntries(std::unordered_map<std::string, NodeDef>& a_scenes,
		const std::vector<ClipLibraryRegistration>& a_registrations, ProblemSink& a_problems,
		std::map<std::string, std::uint32_t>& a_addedByFile, ClipInstalledCache& a_cache);
	void AccumulateFileStats(const std::unordered_map<std::string, NodeDef>& a_scenes,
		ClipInstalledCache& a_cache, std::vector<ContentFileStats>& a_files,
		const std::unordered_map<std::string, std::size_t>& a_index);
}
