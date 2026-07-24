#include "Registry/SceneRegistry.h"

#include "Util/Math.h"  // kDegToRad (offset.heading expectation)

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

	// 20 authored scenes load: bare(1) + legacy(2) + registry(4) + errors(1 of 4) + templates(11) +
	// template-errors(1 of 5); the two malformed-registry files load nothing. (Generated clip-debug
	// entries don't count here.)
	Check(reg.Size() == 20, "authored scene count");

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

	// -- templates: automatic runtime names ------------------------------------------------------
	if (const auto s = reg.Find("test.tpl.mmf")) {
		Check(s->roles.size() == 3, "mmf: role count");
		Check(s->roles[0].name == "m" && s->roles[1].name == "m2" && s->roles[2].name == "f",
			"mmf: a repeated template auto-numbers (m, m2, f)");
		Check(s->roles[1].gender == SlotGender::kMale && s->roles[1].equip.male == "Top.esm|0x111",
			"mmf: the numbered copy keeps the template's fields");
	} else {
		Check(false, "test.tpl.mmf loads");
	}
	if (const auto s = reg.Find("test.tpl.trio")) {
		Check(s->roles.size() == 3 && s->roles[0].name == "m" && s->roles[1].name == "m2" && s->roles[2].name == "m3",
			"trio: three repeats number m, m2, m3");
	} else {
		Check(false, "test.tpl.trio loads");
	}
	if (const auto s = reg.Find("test.tpl.named")) {
		Check(s->roles.size() == 2, "named: role count");
		Check(s->roles[0].name == "lead" && s->roles[0].gender == SlotGender::kMale,
			"named: an object override's explicit name is kept exactly (template fields inherited)");
		Check(s->roles[1].name == "m", "named: the plain ref's automatic name is unaffected");
	} else {
		Check(false, "test.tpl.named loads");
	}
	if (const auto s = reg.Find("test.tpl.collision")) {
		Check(s->roles.size() == 2 && s->roles[0].name == "m2" && s->roles[1].name == "m",
			"collision: the explicit name is reserved first, the automatic slot becomes m2");
	} else {
		Check(false, "test.tpl.collision loads");
	}
	if (const auto s = reg.Find("test.tpl.skip")) {
		Check(s->roles.size() == 3 && s->roles[0].name == "m" && s->roles[1].name == "m2" && s->roles[2].name == "m3",
			"skip: automatic names skip explicitly reserved names");
		Check(s->roles[1].equip.Empty() && s->roles[2].equip.male == "Top.esm|0x111",
			"skip: the middle slot is the inline role, the renumbered one carries the template");
	} else {
		Check(false, "test.tpl.skip loads");
	}

	// -- templates: merge-style overrides ---------------------------------------------------------
	if (const auto s = reg.Find("test.tpl.merge")) {
		Check(s->roles.size() == 1 && s->roles[0].name == "m", "merge: automatic name from the template");
		Check(s->roles[0].gender == SlotGender::kFemale, "merge: a scalar override replaces");
		Check(s->roles[0].offset.x == 1.0f && s->roles[0].offset.y == 9.0f,
			"merge: offset merges by key (y replaced, x retained)");
		Check(s->roles[0].offset.heading == static_cast<float>(90.0 * OSF::Util::kDegToRad),
			"merge: unspecified offset keys (heading) are retained");
		Check(s->roles[0].equip.male == "Top.esm|0x111" && s->roles[0].equip.female == "New.esm|0x999",
			"merge: equip merges by key (female replaced, male retained)");
		Check(s->roles[0].preserveBones.size() == 1 && s->roles[0].preserveBones[0] == "C_GenitalsRoot",
			"merge: an unspecified array is retained");
	} else {
		Check(false, "test.tpl.merge loads");
	}
	if (const auto s = reg.Find("test.tpl.alias1")) {
		Check(s->roles.size() == 1 && s->roles[0].gender == SlotGender::kMale && s->roles[0].name == "geared",
			"alias1: a top-level gender override drops the inherited filters.gender");
		Check(s->roles[0].equip.male == "Suit.esm|0x333" && s->roles[0].equip.any == "Suit.esm|0x444",
			"alias1: unspecified fields are retained");
	} else {
		Check(false, "test.tpl.alias1 loads");
	}
	if (const auto s = reg.Find("test.tpl.alias2")) {
		Check(s->roles.size() == 1 && s->roles[0].gender == SlotGender::kFemale && s->roles[0].name == "m",
			"alias2: a filters.gender override drops the inherited top-level gender");
	} else {
		Check(false, "test.tpl.alias2 loads");
	}
	if (const auto s = reg.Find("test.tpl.bones")) {
		Check(s->roles.size() == 1 && s->roles[0].preserveBones.size() == 1 && s->roles[0].preserveBones[0] == "OnlyThis",
			"bones: an array override replaces the template's array wholesale");
		Check(s->roles[0].gender == SlotGender::kFemale && s->roles[0].name == "geared",
			"bones: unspecified nested fields (filters.gender) are retained");
	} else {
		Check(false, "test.tpl.bones loads");
	}
	if (const auto s = reg.Find("test.tpl.anon")) {
		Check(s->roles.size() == 1 && s->roles[0].name.empty() && s->roles[0].gender == SlotGender::kMale,
			"anon: an explicit name:\"\" stays anonymous (template fields inherited)");
	} else {
		Check(false, "test.tpl.anon loads");
	}
	if (const auto s = reg.Find("test.tpl.null")) {
		Check(s->roles.size() == 1 && s->roles[0].name == "m" && s->roles[0].gender == SlotGender::kMale,
			"null: scalars survive removing optional fields");
		Check(s->roles[0].equip.Empty() && s->roles[0].preserveBones.empty(),
			"null: null removes an inherited optional field");
	} else {
		Check(false, "test.tpl.null loads");
	}

	// -- rejections: only the affected scene, with file + scene + role diagnostics ---------------
	Check(!reg.Find("test.err.unknown"), "unknown reference rejects its scene");
	Check(!reg.Find("test.err.case"), "registry ids are case-sensitive");
	Check(!reg.Find("test.err.dup"), "duplicate explicit runtime role names reject their scene");
	Check(!reg.Find("test.bad.def"), "malformed registry definition rejects its file");
	Check(!reg.Find("test.bad.type"), "non-array/non-object file-level roles rejects its file");
	Check(!reg.Find("test.terr.unknown"), "an unknown object-override id rejects its scene");
	Check(!reg.Find("test.terr.empty"), "an empty id rejects its scene");
	Check(!reg.Find("test.terr.num"), "a non-string id rejects its scene");
	Check(!reg.Find("test.terr.dup"), "duplicate explicit names (override + inline) reject their scene");
	if (const auto s = reg.Find("test.terr.ok")) {
		Check(s->roles.size() == 1 && s->roles[0].name == "f",
			"template error cases reject only their own scene");
	} else {
		Check(false, "test.terr.ok loads");
	}

	const auto errors = reg.LoadErrors();
	for (const auto& e : errors) {
		std::cout << "  diag: " << e << '\n';
	}
	Check(errors.size() == 9, "exactly the nine expected diagnostics");
	CheckError(errors, "'fixture_registry_errors.osf.json': scene 'test.err.unknown': role reference 'nope'",
		"unknown-reference diagnostic carries file + scene + role id");
	CheckError(errors, "scene 'test.err.case': role reference 'F'", "case-sensitive reference diagnostic");
	CheckError(errors, "scene 'test.err.dup': duplicate role name 'f'", "duplicate-name diagnostic");
	CheckError(errors, "'fixture_malformed_def.osf.json': roles registry entry 'bad'", "malformed-definition diagnostic");
	CheckError(errors, "'fixture_malformed_type.osf.json': file-level 'roles' must be an array", "registry type diagnostic");
	CheckError(errors, "'fixture_registry_template_errors.osf.json': scene 'test.terr.unknown': role reference 'nope'",
		"unknown object-override id diagnostic");
	CheckError(errors, "scene 'test.terr.empty': a role object's 'id' must be a non-empty string", "empty-id diagnostic");
	CheckError(errors, "scene 'test.terr.num': a role object's 'id' must be a non-empty string", "non-string-id diagnostic");
	CheckError(errors, "scene 'test.terr.dup': duplicate role name 'lead'", "duplicate explicit-name diagnostic");

	if (g_failures) {
		std::cerr << g_failures << " scene registry test(s) FAILED\n";
		return 1;
	}
	std::cout << "Scene registry tests passed\n";
	return 0;
}
