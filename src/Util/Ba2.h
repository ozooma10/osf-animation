#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// Minimal reader for Starfield BA2 "GNRL" (general) archives — enough to pull a single file
// (e.g. the human skeleton.rig the .af importer needs) out of the base game archives at runtime,
// so OSF doesn't have to ship/redistribute the vanilla asset. Texture (DX10) archives are skipped.
// Format ported from glb2af/tools/ba2extract.py (validated against the shipped archives). Uses zlib.

namespace OSF::Util::Ba2
{
	// Searches every `<cwd>\Data\*.ba2` GNRL archive for `a_relPath` (case-insensitive, separators
	// normalized) and returns its decompressed bytes, or nullopt if no archive contains it.
	// a_relPath is archive-relative with forward slashes, e.g.
	// "meshes/actors/human/characterassets/skeleton.rig".
	std::optional<std::vector<std::uint8_t>> ReadGameFile(std::string_view a_relPath);
}
