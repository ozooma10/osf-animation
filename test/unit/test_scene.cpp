#include "Registry/SceneRegistry.h"

#include <cstdlib>
#include <iostream>
#include <string>

// No game runtime / no mounted archives in this harness: the clip-availability probe reads every
// spec as installed, so the sweep never hides a fixture scene or adds a stray warning.
namespace OSF::Animation
{
	bool ResourceExists(std::string_view)
	{
		return true;
	}
}

namespace
{
	int g_failures = 0;

	void Check(bool a_condition, const char* a_message)
	{
		if (!a_condition) {
			std::cerr << "FAIL: " << a_message << '\n';
			++g_failures;
		}
	}

	// One substring that must appear in SOME load error.
	void CheckError(const std::vector<std::string>& a_errors, const std::string& a_needle, const char* a_message)
	{
		for (const auto& e : a_errors) {
			if (e.find(a_needle) != std::string::npos) {
				return;
			}
		}
		std::cerr << "FAIL: " << a_message << " (missing error containing \"" << a_needle << "\")\n";
		++g_failures;
	}
}

int main()
{
	using OSF::Registry::SceneRegistry;
	using OSF::Registry::SlotGender;

	// xmake runs this target with test/fixtures as cwd, so LoadAll sees Data/OSF.
	auto& reg = SceneRegistry::GetSingleton();
	reg.LoadAll();

	// 8 authored scenes load: bare(1) + legacy(2) + registry(4) + errors(1 of 4); the two
	// malformed-registry files load nothing. (Generated clip-debug entries don't count here.)
	Check(reg.Size() == 8, "authored scene count");

	// -- bare single-scene file: top-level roles is that scene's roles (unchanged) --------------
	if (const auto s = reg.Find("test.bare")) {
		Check(s->roles.size() == 1 && s->roles[0].name == "solo", "bare scene keeps its inline roles");
	} else {
		Check(false, "test.bare loads");
	}

	// -- ARRAY file-level roles: the legacy pack default ----------------------------------------
	if (const auto s = reg.Find("test.legacy.inherit")) {
		Check(s->roles.size() == 2, "legacy inherit: role count");
		Check(s->roles[0].name == "bottom", "legacy inherit: role 0 name");
		Check(s->roles[1].name == "top" && s->roles[1].equip.any == "Any.esm|0x123", "legacy inherit: role 1 name + equip");
	} else {
		Check(false, "test.legacy.inherit loads");
	}
	if (const auto s = reg.Find("test.legacy.override")) {
		Check(s->roles.size() == 1 && s->roles[0].name == "only", "legacy override replaces the pack default");
	} else {
		Check(false, "test.legacy.override loads");
	}

	// -- OBJECT file-level roles: the registry ---------------------------------------------------
	if (const auto s = reg.Find("test.reg.refs")) {
		Check(s->roles.size() == 2, "refs: role count");
		Check(s->roles[0].name == "m", "refs: explicit name overrides the registry id");
		Check(s->roles[0].equip.male == "Robert S Body Replacer.esm|0x804" &&
			s->roles[0].equip.female == "Dick.esm|0x81D", "refs: equip expanded");
		Check(s->roles[1].name == "f", "refs: preserved role name");
		Check(s->roles[1].preserveBones.size() == 1 && s->roles[1].preserveBones[0] == "C_GenitalsRoot",
			"refs: preserveBones expanded");
	} else {
		Check(false, "test.reg.refs loads");
	}
	if (const auto s = reg.Find("test.reg.mixed")) {
		Check(s->roles.size() == 3, "mixed: role count");
		Check(s->roles[0].name == "m", "mixed: ref role 0");
		Check(s->roles[1].name == "f" && s->roles[1].equip.Empty() && s->roles[1].preserveBones.empty(),
			"mixed: omitted name defaults to the registry id");
		Check(s->roles[2].name == "extra" && s->roles[2].offset.y == 1.0f, "mixed: inline object entry");
	} else {
		Check(false, "test.reg.mixed loads");
	}
	if (const auto s = reg.Find("test.reg.infer")) {
		Check(s->roles.size() == 2 && s->roles[0].name.empty() && s->roles[1].name.empty(),
			"registry is not a default cast: omitted roles still infer anonymous slots");
	} else {
		Check(false, "test.reg.infer loads");
	}
	if (const auto s = reg.Find("test.reg.anonymous")) {
		Check(s->roles.size() == 1 && s->roles[0].name.empty(),
			"an explicit empty registry role name stays anonymous");
	} else {
		Check(false, "test.reg.anonymous loads");
	}
	if (const auto s = reg.Find("test.err.ok")) {
		Check(s->roles.size() == 1 && s->roles[0].name == "f", "one bad scene does not reject its file's other scenes");
	} else {
		Check(false, "test.err.ok loads");
	}

	// -- rejections: only the affected scene, with file + scene + role diagnostics ---------------
	Check(!reg.Find("test.err.unknown"), "unknown reference rejects its scene");
	Check(!reg.Find("test.err.case"), "registry ids are case-sensitive");
	Check(!reg.Find("test.err.dup"), "duplicate runtime role names reject their scene");
	Check(!reg.Find("test.bad.def"), "malformed registry definition rejects its file");
	Check(!reg.Find("test.bad.type"), "non-array/non-object file-level roles rejects its file");

	const auto errors = reg.LoadErrors();
	for (const auto& e : errors) {
		std::cout << "  diag: " << e << '\n';
	}
	Check(errors.size() == 5, "exactly the five expected diagnostics");
	CheckError(errors, "'fixture_registry_errors.osf.json': scene 'test.err.unknown': role reference 'nope'",
		"unknown-reference diagnostic carries file + scene + role id");
	CheckError(errors, "scene 'test.err.case': role reference 'F'", "case-sensitive reference diagnostic");
	CheckError(errors, "scene 'test.err.dup': duplicate role name 'f'", "duplicate-name diagnostic");
	CheckError(errors, "'fixture_malformed_def.osf.json': roles registry entry 'bad'", "malformed-definition diagnostic");
	CheckError(errors, "'fixture_malformed_type.osf.json': file-level 'roles' must be an array", "registry type diagnostic");

	if (g_failures) {
		std::cerr << g_failures << " scene registry test(s) FAILED\n";
		return 1;
	}
	std::cout << "Scene registry tests passed\n";
	return 0;
}
