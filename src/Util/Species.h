#pragma once

// Species = skeleton family; each lives under meshes\actors\<species>\ with its own skeleton.rig.

#include <string>
#include <string_view>

namespace RE
{
	class Actor;
}

namespace OSF::Util
{
	// Lowercased folder before "animations" under actors/, e.g. ".../sfbgs004/modela/animations/x.af" -> "modela".
	// "" when the path has no actors/<species>/animations shape.
	std::string SpeciesFromAnimPath(std::string_view a_animPath);

	// "<species folder>\characterassets\skeleton.rig"; "" when not an actors/<species>/animations path.
	std::string RigResourcePathFromAnimPath(std::string_view a_animPath);

	bool IsKnownSpecies(std::string_view a_species);

	// From the race skeleton-model path; "human" when unclassified, "" only with no actor.
	std::string ActorSpecies(RE::Actor* a_actor);
}
