#pragma once

// Species (skeleton family) helpers. Starfield keeps each actor kind's animations and its
// own skeleton.rig under meshes\actors\<species>\ (human, terrormorph, bipeda, …; bone counts
// run 6 -> 123). OSF derives, from a clip's OWN path, both the rig to decode its .af against
// (playback) and the species the clip belongs to (the browser filters the catalog to the
// picked actor's species). One source of truth for that path -> species mapping.

#include <string>
#include <string_view>

namespace RE
{
	class Actor;
}

namespace OSF::Util
{
	// Skeleton family from a data-relative animation path — the innermost actor folder before
	// the "animations" segment, lowercased. Slash-agnostic.
	//   "meshes/actors/bipeda/animations/x.af"          -> "bipeda"
	//   "meshes\actors\sfbgs004\modela\animations\x.af" -> "modela"
	//   "meshes/actors/human/animations/x.af"           -> "human"
	// Returns "" when the path is not an actors/<species>/animations clip (loose / NAF clips).
	std::string SpeciesFromAnimPath(std::string_view a_animPath);

	// The skeleton.rig resource path (engine form, backslashes) the .af at a_animPath must decode
	// against: the folder that holds "animations", plus "\characterassets\skeleton.rig".
	//   "meshes/actors/bipeda/animations/x.af" -> "meshes\actors\bipeda\characterassets\skeleton.rig"
	// "" when a_animPath carries no actors/<species>/animations shape (caller keeps the human default).
	std::string RigResourcePathFromAnimPath(std::string_view a_animPath);

	// Is a_species one of the vanilla skeleton families OSF ships packs for? Guards actor-species
	// detection against reading an unrelated path out of an engine field.
	bool IsKnownSpecies(std::string_view a_species);

	// The skeleton family of a live actor, for the browser's species filter. Resolved from the
	// actor's race skeleton-model path (validated by IsKnownSpecies), defaulting to "human" for
	// ordinary/unclassified actors. RE-sensitive (race model field offset) — logs its result at
	// DEBUG so a misread can be seen in-game; "" only when there is no actor/race at all.
	std::string ActorSpecies(RE::Actor* a_actor);
}
