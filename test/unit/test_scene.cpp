#include "Registry/SceneRegistry.h"

#include "Util/Math.h"  // kDegToRad (offset.heading expectation)

#include <cstdlib>
#include <iostream>
#include <limits>
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
	using OSF::Animation::PoseMode;

	// xmake runs this target with test/fixtures as cwd, so LoadAll sees Data/OSF.
	Check(OSF::Registry::ParseCameraState("Scene_Orbit") == OSF::Registry::CameraState::kSceneOrbit,
		"camera state parse is case-insensitive and typed");
	Check(OSF::Registry::CameraStateName(OSF::Registry::CameraState::kThirdPersonHold) == "thirdperson_hold",
		"camera state serializes to its canonical name");
	Check(!OSF::Registry::ParseCameraState("cinematic"),
		"unknown camera state is rejected");

	auto& reg = SceneRegistry::GetSingleton();
	reg.LoadAll();

	// 22 authored scenes load: bare(1) + legacy(2) + registry(5) + errors(1 of 10) + templates(11) +
	// template-errors(1 of 5); the two malformed-registry files load nothing. (Generated clip-debug
	// entries and clipLibrary registrations don't count here.)
	Check(reg.Size() == 22, "authored scene count");

	// -- explicit clip library: friendly metadata wins over automatic filename discovery ---------
	std::int32_t curatedCount = 0;
	std::int32_t curatedFlagged = 0;   // registered entries carrying curatedClip
	std::int32_t harvestedFlagged = 0;  // entries harvested from a scene's stages (curatedClip false)
	bool friendlyFound = false;
	bool fallbackFound = false;
	bool animatedFound = false;
	bool clipOnlyFound = false;
	std::int32_t duplicateCount = 0;
	reg.ForEachDef([&](const OSF::Registry::SceneDef& d) {
		if (d.library && d.pack == "Test Clip Library") {
			++curatedCount;
			if (d.name == "Friendly Pose") {
				friendlyFound = d.folder == "Standing/Heroic" && d.unlisted && d.inPlace &&
					!d.lockPlayer && !d.stripActors &&
					d.tagSet.contains("scene.clip") && d.tagSet.contains("pose") && d.tagSet.contains("standing") &&
					d.nodes.size() == 1 && d.nodes[0].stages.size() == 1 &&
					d.nodes[0].stages[0].name == "Friendly Pose" &&
					d.nodes[0].stages[0].clips.size() == 1 &&
					d.nodes[0].stages[0].clips[0].file == "clips/test/friendly.af";
			} else if (d.name == "fallback.af") {
				fallbackFound = d.folder == "Poses";
			} else if (d.name == "Looking Away") {
				animatedFound = d.folder == "Poses" && d.nodes.size() == 1 &&
					d.nodes[0].stages.size() == 1 &&
					d.nodes[0].stages[0].clips.size() == 1 &&
					d.nodes[0].stages[0].clips[0].animId == "LookAway";
			}
		}
		if (d.library && d.pack == "Test Clip Only" && d.name == "Only Pose") {
			clipOnlyFound = true;
		}
		if (d.library && d.pack == "Test Clip Duplicate") {
			++duplicateCount;
		}
		// The browser shows registered clips to everyone and keeps harvested ones behind author
		// details, and `curatedClip` is the ONLY thing that separates them — a registration may
		// carry no name, folder or tags at all, so nothing else in the def can stand in for it.
		if (d.library && d.tagSet.contains("scene.clip")) {
			(d.curatedClip ? curatedFlagged : harvestedFlagged) += 1;
		}
	});
	Check(curatedCount == 3, "registered clip de-duplicates against the same scene-referenced clip");
	Check(friendlyFound, "clipLibrary friendly name, folder override, tags, clipRoot, and safe playback posture");
	Check(fallbackFound, "bare clipLibrary entry falls back to filename and inherits file folder");
	Check(animatedFound, "clipLibrary object preserves GLB animation id");
	Check(clipOnlyFound, "clipLibrary-only file loads without a dummy scene");

	Check(duplicateCount == 1, "duplicate explicit clip registration keeps the first entry");
	// 5 registrations survive across the fixtures (3 in Test Clip Library, 1 Only, 1 Duplicate).
	Check(curatedFlagged == 5, "every clipLibrary registration is flagged curated");
	Check(harvestedFlagged > 0, "clips harvested from scene stages are NOT flagged curated");
	// -- bare single-scene file: top-level roles is that scene's roles (unchanged) --------------
	if (const auto s = reg.Find("test.bare")) {
		Check(s->roles.size() == 1 && s->roles[0].name == "solo", "bare scene keeps its inline roles");
		Check(s->roles[0].poseMode == PoseMode::kOverride && s->roles[0].poseWeight == 1.0f,
			"omitted pose policy defaults to override at full weight");
		Check(s->clearHeldItems, "omitted clearHeldItems policy defaults on");
	} else {
		Check(false, "test.bare loads");
	}

	// -- ARRAY file-level roles: the legacy pack default ----------------------------------------
	if (const auto s = reg.Find("test.legacy.inherit")) {
		Check(s->roles.size() == 2, "legacy inherit: role count");
		Check(s->roles[0].name == "bottom", "legacy inherit: role 0 name");
		Check(!s->clearHeldItems, "legacy inherit: file-level clearHeldItems default");
		Check(s->roles[1].name == "top" && s->roles[1].equip.any == "Any.esm|0x123", "legacy inherit: role 1 name + equip");
	} else {
		Check(false, "test.legacy.inherit loads");
	}
	if (const auto s = reg.Find("test.legacy.override")) {
		Check(s->roles.size() == 1 && s->roles[0].name == "only", "legacy override replaces the pack default");
		Check(s->clearHeldItems, "legacy override: scene-level clearHeldItems wins");
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
	if (const auto s = reg.Find("test.reg.props")) {
		Check(s->nodes.size() == 1 && s->nodes[0].actions.size() == 2,
			"prop scene keeps its attach and destroy actions");
		if (s->nodes.size() == 1 && s->nodes[0].actions.size() == 2) {
			const auto& attach = s->nodes[0].actions[0];
			const auto& destroy = s->nodes[0].actions[1];
			Check(attach.type == "osf.prop.attach" && attach.role == "player" &&
				attach.prop == "helmet", "prop attach identity and role parse");
			Check(attach.kind == OSF::Registry::ActionKind::kPropAttach, "prop attach parses to its action kind");
			Check(destroy.kind == OSF::Registry::ActionKind::kPropDestroy, "prop destroy parses to its action kind");
			Check(attach.propSource.kind == OSF::Props::SourceKind::kEquippedArmor &&
				attach.propSource.keywords.size() == 2 &&
				attach.propSource.keywords[0] == "ArmorTypeSpacesuitHelmet",
				"equipped-armor prop source parses its keyword fallbacks");
			Check(attach.propAttachment.node == "R_Wrist" &&
				attach.propAttachment.position[0] == 1.0f &&
				attach.propAttachment.position[1] == 2.0f &&
				attach.propAttachment.position[2] == 3.0f &&
				attach.propAttachment.rotation[1] == -90.0f &&
				attach.propAttachment.scale == 0.75f,
				"prop node and local transform parse");
			Check(destroy.type == "osf.prop.destroy" && destroy.prop == "helmet",
				"prop destroy addresses the scene-local id");
		}
	} else {
		Check(false, "test.reg.props loads");
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
		Check(s->roles[0].poseMode == PoseMode::kAdditive && s->roles[1].poseMode == PoseMode::kAdditive &&
			s->roles[0].poseWeight == 0.75f && s->roles[1].poseWeight == 0.75f &&
			s->roles[2].poseMode == PoseMode::kOverride && s->roles[2].poseWeight == 1.0f,
			"mmf: additive pose policy is inherited by template copies while omission stays override");
		Check(s->roles[0].mask == "upperBody" && s->roles[1].mask == "upperBody" && s->roles[2].mask.empty(),
			"mmf: an authored-lowercase mask canonicalizes and inherits; omission stays unmasked");
		const auto plan = reg.BuildNodePlan(s, s->nodes[0], 3);
		Check(plan.has_value(), "mmf: scene plan builds");
		if (plan) {
			Check(plan->poseModes.size() == 3 && plan->poseWeights.size() == 3 && plan->roleNames.size() == 3,
				"mmf: scene plan carries one pose policy and role name per actor");
			Check(plan->poseModes[0] == PoseMode::kAdditive && plan->poseModes[1] == PoseMode::kAdditive &&
				plan->poseModes[2] == PoseMode::kOverride && plan->poseWeights[0] == 0.75f &&
				plan->poseWeights[1] == 0.75f && plan->poseWeights[2] == 1.0f,
				"mmf: scene plan preserves resolved role pose values");
			Check(plan->masks.size() == 3 && plan->masks[0] == "upperBody" &&
				plan->masks[1] == "upperBody" && plan->masks[2].empty(),
				"mmf: scene plan carries one bone mask per actor");
			Check(OSF::Animation::HasValidRolePolicyShape(*plan, 3), "mmf: matching policy arrays validate");
			auto badModes = *plan;
			badModes.poseModes.pop_back();
			Check(!OSF::Animation::HasValidRolePolicyShape(badModes, 3),
				"scene-plan validation rejects a pose-mode/actor count mismatch");
			auto badWeights = *plan;
			badWeights.poseWeights.pop_back();
			Check(!OSF::Animation::HasValidRolePolicyShape(badWeights, 3),
				"scene-plan validation rejects a pose-weight/actor count mismatch");
			auto badMasks = *plan;
			badMasks.masks.pop_back();
			Check(!OSF::Animation::HasValidRolePolicyShape(badMasks, 3),
				"scene-plan validation rejects a mask/actor count mismatch");
			auto badMaskName = *plan;
			badMaskName.masks[0] = "torso";
			Check(!OSF::Animation::HasValidRolePolicyShape(badMaskName, 3),
				"scene-plan validation rejects an unknown mask name");
		}
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
		Check(s->roles[0].poseMode == PoseMode::kOverride && s->roles[0].poseWeight == 0.0f,
			"merge: pose scalars replace and a low poseWeight clamps to zero");
		Check(s->roles[0].mask == "arms", "merge: a mask override replaces the template's mask");
	} else {
		Check(false, "test.tpl.merge loads");
	}
	if (const auto s = reg.Find("test.tpl.alias1")) {
		Check(s->roles.size() == 1 && s->roles[0].gender == SlotGender::kMale && s->roles[0].name == "geared",
			"alias1: a top-level gender override drops the inherited filters.gender");
		Check(s->roles[0].equip.male == "Suit.esm|0x333" && s->roles[0].equip.any == "Suit.esm|0x444",
			"alias1: unspecified fields are retained");
		Check(s->roles[0].poseMode == PoseMode::kOverride && s->roles[0].poseWeight == 1.0f,
			"alias1: a high poseWeight clamps to one without changing the default mode");
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
		Check(s->roles[0].equip.Empty() && s->roles[0].preserveBones.empty() && s->roles[0].mask.empty(),
			"null: null removes an inherited optional field");
	} else {
		Check(false, "test.tpl.null loads");
	}

	// -- rejections: only the affected scene, with file + scene + role diagnostics ---------------
	Check(!reg.Find("test.err.unknown"), "unknown reference rejects its scene");
	Check(!reg.Find("test.err.case"), "registry ids are case-sensitive");
	Check(!reg.Find("test.err.dup"), "duplicate explicit runtime role names reject their scene");
	Check(!reg.Find("test.err.pose-mode"), "unknown poseMode rejects its scene");
	Check(!reg.Find("test.err.pose-mode-type"), "wrong-type poseMode rejects its scene");
	Check(!reg.Find("test.err.pose-weight-type"), "wrong-type poseWeight rejects its scene");
	Check(!reg.Find("test.err.mask"), "unknown mask rejects its scene");
	Check(!reg.Find("test.err.mask-type"), "wrong-type mask rejects its scene");
	Check(!reg.Find("test.err.clear-held-type"), "wrong-type clearHeldItems rejects its scene");
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
	Check(errors.size() == 17, "exactly the seventeen expected diagnostics");
	CheckError(errors, "'fixture_registry_errors.osf.json': scene 'test.err.unknown': role reference 'nope'",
		"unknown-reference diagnostic carries file + scene + role id");
	CheckError(errors, "scene 'test.err.case': role reference 'F'", "case-sensitive reference diagnostic");
	CheckError(errors, "scene 'test.err.dup': duplicate role name 'f'", "duplicate-name diagnostic");
	CheckError(errors, "scene 'test.err.pose-mode': role 'p': unknown 'poseMode' value 'multiply'",
		"unknown poseMode diagnostic names the scene, role, value, and field");
	CheckError(errors, "scene 'test.err.pose-mode-type': role 'p': 'poseMode' must be a string",
		"wrong-type poseMode diagnostic names the scene, role, and contract");
	CheckError(errors, "scene 'test.err.pose-weight-type': role 'p': 'poseWeight' must be a finite number",
		"wrong-type poseWeight diagnostic names the scene, role, and contract");
	CheckError(errors, "scene 'test.err.mask': role 'p': unknown 'mask' value 'torso'",
		"unknown mask diagnostic names the scene, role, and value");
	CheckError(errors, "scene 'test.err.mask-type': role 'p': 'mask' must be a string",
		"wrong-type mask diagnostic names the scene, role, and contract");
	CheckError(errors, "scene 'test.err.clear-held-type': 'clearHeldItems' must be a boolean",
		"wrong-type clearHeldItems diagnostic names the scene and contract");
	CheckError(errors, "'fixture_malformed_def.osf.json': roles registry entry 'bad'", "malformed-definition diagnostic");
	CheckError(errors, "'fixture_malformed_type.osf.json': file-level 'roles' must be an array", "registry type diagnostic");
	CheckError(errors, "'fixture_registry_template_errors.osf.json': scene 'test.terr.unknown': role reference 'nope'",
		"unknown object-override id diagnostic");
	CheckError(errors, "scene 'test.terr.empty': a role object's 'id' must be a non-empty string", "empty-id diagnostic");
	CheckError(errors, "scene 'test.terr.num': a role object's 'id' must be a non-empty string", "non-string-id diagnostic");
	CheckError(errors, "scene 'test.terr.dup': duplicate role name 'lead'", "duplicate explicit-name diagnostic");
	CheckError(errors, "duplicate clipLibrary registration for",
		"duplicate clipLibrary diagnostic names the registered clip");
	CheckError(errors, "'folder' contains an empty segment",
		"invalid clipLibrary folder diagnostic names the path problem");
	Check(!OSF::Animation::NormalizePoseWeight(std::numeric_limits<double>::quiet_NaN()).has_value(),
		"non-finite poseWeight normalization rejects NaN");

	// -- per-file import records (the browser's IMPORTS listing) --------------------------------
	{
		const auto files = reg.FileStats();
		Check(files.size() == 12, "one import record per discovered *.osf.json");

		const auto find = [&files](std::string_view a_name) -> const OSF::Registry::SceneFileStats* {
			for (const auto& f : files) {
				if (f.file == a_name) {
					return &f;
				}
			}
			return nullptr;
		};

		std::uint32_t accepted = 0;
		std::uint32_t owned = 0;
		bool sorted = true;
		bool pathsRelative = true;
		for (std::size_t i = 0; i < files.size(); ++i) {
			accepted += files[i].scenes;
			owned += static_cast<std::uint32_t>(files[i].problems.size());
			if (i && files[i - 1].path > files[i].path) {
				sorted = false;
			}
			// Never an absolute path, and always forward-slashed.
			if (files[i].path.find(':') != std::string::npos || files[i].path.find('\\') != std::string::npos) {
				pathsRelative = false;
			}
		}
		Check(accepted == 22, "per-file scene counts sum to the authored total");
		Check(owned == errors.size(), "every load problem is attributed to exactly one file");
		Check(sorted, "import records are sorted by path");
		Check(pathsRelative, "import record paths are Data/OSF-relative and forward-slashed");

		const auto* bare = find("fixture_bare.osf.json");
		Check(bare && bare->scenes == 1, "the bare single-scene fixture reports one scene");
		Check(bare && bare->schema == OSF::Registry::kSchemaVersion, "the declared schema is recorded");
		Check(bare && bare->bytes > 0, "the file size is recorded");
		Check(bare && bare->declaredScenes == 1 && bare->rejectedScenes == 0,
			"clean files report exact authored and rejected scene counts");
		Check(bare && bare->nodes > 0 && bare->stages > 0 && bare->clips > 0,
			"node/stage/clip totals are recorded");
		Check(bare && bare->distinctClips > 0 && bare->distinctClips <= bare->clips,
			"distinct clips are counted and never exceed the clip slots");
		Check(bare && bare->errors == 0 && bare->warnings == 0, "a clean file reports no problems");
		Check(bare && !bare->Rejected(), "a clean file is not flagged rejected");

		// Whole-file reject: the record still exists, which is the whole point — a pack that
		// contributed nothing has to be visible as such, not simply absent.
		const auto* malformed = find("fixture_malformed_type.osf.json");
		Check(malformed && malformed->scenes == 0 && malformed->errors == 1,
			"a rejected file keeps a record carrying its reject line");
		Check(malformed && malformed->declaredScenes == 1 && malformed->rejectedScenes == 1,
			"whole-file rejection preserves its authored scene count");
		Check(malformed && malformed->problems[0].code == "file-invalid" && !malformed->problems[0].hint.empty(), "rejected files carry structured repair guidance");
		Check(malformed && malformed->Rejected(), "a file that contributed nothing is flagged rejected");

		// Partial file: some scenes in, nine rejected, so it is NOT "rejected".
		const auto* partial = find("fixture_registry_errors.osf.json");
		Check(partial && partial->scenes == 1 && partial->errors == 9,
			"a partially-loaded file reports both its scenes and its rejected ones");
		Check(partial && partial->declaredScenes == 10 && partial->rejectedScenes == 9,
			"partial files report exact authored and rejected scene counts");
		Check(partial && std::ranges::all_of(partial->problems, [](const auto& a_problem) {
			return !a_problem.code.empty() && !a_problem.hint.empty(); }), "scene diagnostics carry structured codes and repair hints");
		Check(partial && !partial->Rejected(), "a file that loaded something is not flagged rejected");

		const auto* clipLib = find("fixture_clip_library.osf.json");
		Check(clipLib && clipLib->clipEntries > 0, "generated clip entries are attributed to their file");
		Check(clipLib && clipLib->pack == "Test Clip Library", "the file-level pack label is recorded");

		// A clipLibrary-ONLY file declares no scenes, so its contribution is visible only through
		// the generated-entry count — and that alone has to keep it off the rejected list.
		const auto* clipOnly = find("fixture_clip_only.osf.json");
		Check(clipOnly && clipOnly->scenes == 0 && clipOnly->clipEntries > 0,
			"a clipLibrary-only file reports zero scenes and its generated entries");
		Check(clipOnly && !clipOnly->Rejected(), "a file contributing only clip entries is not rejected");
	}

	if (g_failures) {
		std::cerr << g_failures << " scene registry test(s) FAILED\n";
		return 1;
	}
	std::cout << "Scene registry tests passed\n";
	return 0;
}
