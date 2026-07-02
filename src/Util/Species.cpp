#include "Util/Species.h"

#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace OSF::Util
{
	namespace
	{
		// Lowercase + forward-slash a path so parsing is case- and separator-insensitive.
		std::string Normalize(std::string_view a_path)
		{
			std::string s(a_path);
			for (auto& c : s) {
				if (c == '\\') {
					c = '/';
				} else {
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
			}
			return s;
		}

		std::vector<std::string> Split(const std::string& a_s)
		{
			std::vector<std::string> out;
			std::size_t              start = 0;
			for (std::size_t i = 0; i <= a_s.size(); ++i) {
				if (i == a_s.size() || a_s[i] == '/') {
					if (i > start) {
						out.emplace_back(a_s.substr(start, i - start));
					}
					start = i + 1;
				}
			}
			return out;
		}

		// Index of a_marker in a_segs that has an "actors" segment before it, or -1. The species
		// (skeleton family) is the segment immediately before that marker.
		int MarkerIndex(const std::vector<std::string>& a_segs, std::string_view a_marker)
		{
			bool haveActors = false;
			for (std::size_t i = 0; i < a_segs.size(); ++i) {
				if (a_segs[i] == "actors") {
					haveActors = true;
				} else if (haveActors && i > 0 && a_segs[i] == a_marker) {
					return static_cast<int>(i);
				}
			}
			return -1;
		}
	}

	std::string SpeciesFromAnimPath(std::string_view a_animPath)
	{
		const auto segs = Split(Normalize(a_animPath));
		const int  idx = MarkerIndex(segs, "animations");
		return idx > 0 ? segs[static_cast<std::size_t>(idx) - 1] : std::string{};
	}

	std::string RigResourcePathFromAnimPath(std::string_view a_animPath)
	{
		const auto segs = Split(Normalize(a_animPath));
		const int  idx = MarkerIndex(segs, "animations");
		if (idx <= 0) {
			return {};
		}
		std::string rig;
		for (int i = 0; i < idx; ++i) {
			rig += segs[static_cast<std::size_t>(i)];
			rig += '\\';
		}
		rig += "characterassets\\skeleton.rig";
		return rig;
	}

	bool IsKnownSpecies(std::string_view a_species)
	{
		// The vanilla skeleton families OSF ships packs for (generate_vanilla_packs.py). "human"
		// is here so a human actor validates too. Kept in sync with the generated pack roots.
		static constexpr std::array kKnown{
			std::string_view{ "human" }, std::string_view{ "terrormorph" },
			std::string_view{ "modela" }, std::string_view{ "models" }, std::string_view{ "modelt" },
			std::string_view{ "bipeda" }, std::string_view{ "hexapoda" }, std::string_view{ "hoppera" },
			std::string_view{ "quadrupeda" }, std::string_view{ "quadrupedb" }, std::string_view{ "quadrupedc" },
			std::string_view{ "octopedea" }, std::string_view{ "mantida" }, std::string_view{ "mantaa" },
			std::string_view{ "parasitea" }, std::string_view{ "minibota" }, std::string_view{ "critter" },
			std::string_view{ "swimmera" }, std::string_view{ "floatera" }, std::string_view{ "flyera" },
			std::string_view{ "larvaa" }, std::string_view{ "anemonea" }, std::string_view{ "ballisticturret" },
			std::string_view{ "laserturret" }, std::string_view{ "securitycamera" }, std::string_view{ "mannequin" },
		};
		for (const auto& s : kKnown) {
			if (s == a_species) {
				return true;
			}
		}
		return false;
	}

	std::string ActorSpecies(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return {};
		}
		RE::TESRace* race = a_actor->race;
		if (!race) {
			return "human";  // no race -> assume the common case
		}

		// The species is the actor folder before "characterassets" in the race's skeleton-model
		// path. The exact TESRace model field is build-sensitive, so scan every model slot and
		// accept only a path that resolves to a KNOWN species — a wrong field just yields nothing.
		const auto speciesOf = [](const char* a_model) -> std::string {
			if (!a_model || !a_model[0]) {
				return {};
			}
			const auto segs = Split(Normalize(a_model));
			const int  idx = MarkerIndex(segs, "characterassets");
			if (idx <= 0) {
				return {};
			}
			std::string sp = segs[static_cast<std::size_t>(idx) - 1];
			return IsKnownSpecies(sp) ? sp : std::string{};
		};

		std::string species;
		for (const auto& model : race->unk5E8) {  // TESModel[4]: skeleton/body models
			if (auto sp = speciesOf(model.model.c_str()); !sp.empty()) {
				species = std::move(sp);
				break;
			}
		}

		const char* raceEdid = race->GetFormEditorID();
		if (species.empty()) {
			species = "human";  // unclassified -> treat as human (the browser's default lane)
		}
		REX::DEBUG("[UI] actor {:08X} race {:08X} '{}' -> species '{}'",
			a_actor->GetFormID(), race->GetFormID(), raceEdid ? raceEdid : "", species);
		return species;
	}
}
