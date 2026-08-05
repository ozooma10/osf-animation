#include "Check.h"

#include "Registry/SceneRegistry.h"
#include "Scene/InspectionPropTimeline.h"

#include "Animation/Scene.h"  // the frozen-stage clock (`hold`)
#include "Util/Math.h"        // kDegToRad (offset.heading expectation)

#include <iostream>
#include <limits>
#include <string>
#include <vector>

using OSF::Test::Check;
using OSF::Test::Finish;

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
	// One substring that must appear in SOME load error.
	void CheckError(const std::vector<std::string>& a_errors, const std::string& a_needle, const char* a_message)
	{
		for (const auto& e : a_errors) {
			if (e.find(a_needle) != std::string::npos) {
				return;
			}
		}
		const auto detail = std::string{ a_message } + " (missing error containing \"" + a_needle + "\")";
		Check(false, detail);
	}
}

int main()
{
	using OSF::Registry::SceneRegistry;
	using OSF::Registry::ActionKind;
	using OSF::Registry::LoopMode;
	using OSF::Registry::SlotGender;
	using OSF::Registry::SoundEmitter;
	using OSF::Registry::TrackPos;
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
	{
		const auto route = reg.FindRoute("FIXTURE.OVERLAY.HELMET");
		Check(route && route->stations.size() == 3 && route->transitions.size() == 3,
			"overlay routes load into the case-insensitive snapshot map");
		Check(route && !route->FindStation("head")->layer && route->FindStation("held")->layer.has_value(),
			"zero-animation and animated stations retain their distinct shapes");
		const auto* edge = route ? route->FindTransition("head-to-held") : nullptr;
		Check(edge && edge->layer.mask == "arms" && edge->commit &&
			edge->commit->id == "fixture.helmet.hide_head" && edge->commit->frame == 24.0f && edge->props.size() == 2,
			"transition layer, commit, and prop contracts are retained");
		Check(edge && edge->reach && edge->reach->targetBone == "C_Head" &&
			edge->reach->carrierBone == "R_AnimObject1" && edge->reach->secondaryLimbs.size() == 1 &&
			edge->reach->tracking == OSF::Animation::ReachTracking::kFreezeAtContact &&
			std::abs(edge->reach->maxCorrection - 0.25f) < 0.0001f,
			"transition live reach contract is validated and retained");
		Check(edge && edge->props[0].lifetime == OSF::Registry::RouteLifetime::kExternal &&
			edge->props[1].lifetime == OSF::Registry::RouteLifetime::kStation && edge->props[1].attachment.node == "C_Head",
			"external props need no OSF source while owned props resolve the file-local template");
	}

	// 35 authored scenes load: the prior 22, the two compiled-route contract scenes, the
	// prop-transform-default scene, the `atFrame` scene, the five props-registry scenes, the bare
	// props file, the one surviving scene of the props reference-error fixture, and the two valid
	// `hold` scenes (that fixture's two malformed ones are rejected). The compiled-route error
	// fixture and the four malformed-registry files load nothing. (Generated clip-debug entries and
	// clipLibrary registrations don't count here.)
	Check(reg.Size() == 35, "authored scene count");

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
		if (Check(s->roles.size() == 1, "bare scene keeps one inline role")) {
			Check(s->roles[0].name == "solo", "bare scene keeps its inline role name");
			Check(s->roles[0].poseMode == PoseMode::kOverride && s->roles[0].poseWeight == 1.0f,
				"omitted pose policy defaults to override at full weight");
		}
		Check(s->clearHeldItems, "omitted clearHeldItems policy defaults on");
	} else {
		Check(false, "test.bare loads");
	}

	// -- ARRAY file-level roles: the legacy pack default ----------------------------------------
	if (const auto s = reg.Find("test.legacy.inherit")) {
		if (Check(s->roles.size() == 2, "legacy inherit: role count")) {
			Check(s->roles[0].name == "bottom", "legacy inherit: role 0 name");
			Check(s->roles[1].name == "top" && s->roles[1].equip.any == "Any.esm|0x123",
				"legacy inherit: role 1 name + equip");
		}
		Check(!s->clearHeldItems, "legacy inherit: file-level clearHeldItems default");
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
		if (Check(s->roles.size() == 2, "refs: role count")) {
			Check(s->roles[0].name == "m", "refs: explicit name overrides the registry id");
			Check(s->roles[0].equip.male == "Robert S Body Replacer.esm|0x804" &&
				s->roles[0].equip.female == "Dick.esm|0x81D", "refs: equip expanded");
			Check(s->roles[1].name == "f", "refs: preserved role name");
			Check(s->roles[1].preserveBones.size() == 1 && s->roles[1].preserveBones[0] == "C_GenitalsRoot",
				"refs: preserveBones expanded");
		}
	} else {
		Check(false, "test.reg.refs loads");
	}
	if (const auto s = reg.Find("test.reg.mixed")) {
		if (Check(s->roles.size() == 3, "mixed: role count")) {
			Check(s->roles[0].name == "m", "mixed: ref role 0");
			Check(s->roles[1].name == "f" && s->roles[1].equip.Empty() && s->roles[1].preserveBones.empty(),
				"mixed: omitted name defaults to the registry id");
			Check(s->roles[2].name == "extra" && s->roles[2].offset.y == 1.0f,
				"mixed: inline object entry");
		}
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
		Check(s->nodes.size() == 1 && s->nodes[0].actions.size() == 3 && s->nodes[0].sounds.size() == 3,
			"prop scene keeps its actions and expanded sound marks");
		if (s->nodes.size() == 1 && s->nodes[0].actions.size() == 3 && s->nodes[0].sounds.size() == 3) {
			const auto& attach = s->nodes[0].actions[0];
			const auto& destroy = s->nodes[0].actions[1];
			const auto& voice = s->nodes[0].actions[2];
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
			Check(voice.kind == ActionKind::kVoicePlay && voice.role == "player" &&
				voice.set == "Sound/OSF/test-voice.wem" && voice.emitter == SoundEmitter::kRole,
				"osf.voice.play parses its positioned role emitter");
			Check(s->nodes[0].sounds[0].spec == "Sound/OSF/test-flat.wem" &&
				s->nodes[0].sounds[0].emitter == SoundEmitter::kRole,
				"flat sound parses its positioned role emitter");
			Check(s->nodes[0].sounds[1].spec == "$test,voice" &&
				s->nodes[0].sounds[1].emitter == SoundEmitter::kRole &&
				s->nodes[0].sounds[2].emitter == SoundEmitter::kListener,
				"sound ladder inherits and per-mark overrides its emitter");
		}
	} else {
		Check(false, "test.reg.props loads");
	}
	// An attach that omits the local transform is identical to authoring the identity transform,
	// so authors can drop all three keys.
	if (const auto s = reg.Find("test.reg.props.defaults")) {
		if (s->nodes.size() == 1 && s->nodes[0].actions.size() == 1) {
			const auto& attach = s->nodes[0].actions[0];
			Check(attach.propAttachment.position == std::array<float, 3>{ 0.0f, 0.0f, 0.0f } &&
				attach.propAttachment.rotation == std::array<float, 3>{ 0.0f, 0.0f, 0.0f } &&
				attach.propAttachment.scale == 1.0f,
				"omitted prop position/rotation/scale default to identity");
		} else {
			Check(false, "test.reg.props.defaults keeps its single attach action");
		}
	} else {
		Check(false, "test.reg.props.defaults loads");
	}

	// -- file-level `props` registry: templates desugar at load, inline keys keep the last word ----
	// The registry is a pure parse-time desugar, so every assertion below reads the SAME
	// propSource/propAttachment fields an inline attach produces — nothing downstream can tell.
	const auto firstAttach = [&](const char* a_scene) -> const OSF::Registry::ActionEntry* {
		const auto s = reg.Find(a_scene);
		if (!s || s->nodes.size() != 1 || s->nodes[0].actions.empty()) {
			return nullptr;
		}
		return &s->nodes[0].actions[0];
	};

	if (const auto* attach = firstAttach("test.props.ref")) {
		Check(attach->prop == "helmet" && attach->kind == ActionKind::kPropAttach &&
			attach->propSource.kind == OSF::Props::SourceKind::kEquippedArmor &&
			attach->propSource.keywords.size() == 2 &&
			attach->propSource.keywords[0] == "ArmorTypeSpacesuitHelmet",
			"a bare 'prop' id resolves its file-level template's source");
		Check(attach->propAttachment.node == "R_AnimObject1" &&
			attach->propAttachment.rotation[1] == -90.0f &&
			attach->propAttachment.scale == 0.75f,
			"a template supplies node and transform the action never authored");
		const auto s = reg.Find("test.props.ref");
		Check(s->nodes[0].actions.size() == 2 &&
			s->nodes[0].actions[1].kind == ActionKind::kPropDestroy &&
			s->nodes[0].actions[1].prop == "helmet",
			"destroy still addresses the scene-local id, untouched by the registry");
	} else {
		Check(false, "test.props.ref loads with its attach");
	}

	if (const auto* attach = firstAttach("test.props.use")) {
		// `prop` stays the RUNTIME id; `use` only picks which definition to copy.
		Check(attach->prop == "helmet_l" && attach->propAttachment.node == "L_AnimObject1",
			"'use' names the template while 'prop' remains the runtime id");
		Check(attach->propSource.kind == OSF::Props::SourceKind::kEquippedArmor &&
			attach->propAttachment.scale == 0.75f,
			"a 'use' reference inherits every key it does not override");
	} else {
		Check(false, "test.props.use loads with its attach");
	}

	// The deep-merge regression: an inline `source` must REPLACE the template's selector outright.
	// A key-by-key merge would leave both `form` and `equippedArmor` set and fail to parse.
	if (const auto* attach = firstAttach("test.props.source")) {
		Check(attach->propSource.kind == OSF::Props::SourceKind::kForm &&
			attach->propSource.form == "Fixture.esm|0x801" &&
			attach->propSource.keywords.empty(),
			"an inline source replaces the inherited selector whole");
		Check(attach->propAttachment.node == "R_AnimObject1",
			"overriding source still inherits the template's other keys");
	} else {
		Check(false, "test.props.source loads with its attach");
	}

	if (const auto* attach = firstAttach("test.props.partial")) {
		Check(attach->propSource.kind == OSF::Props::SourceKind::kForm &&
			attach->propAttachment.node == "R_Wrist" && attach->propAttachment.scale == 1.0f,
			"a partial template is completed by the action, validated only once merged");
	} else {
		Check(false, "test.props.partial loads with its attach");
	}

	if (const auto* attach = firstAttach("test.props.inline")) {
		Check(attach->propSource.kind == OSF::Props::SourceKind::kEquippedArmor &&
			attach->propAttachment.node == "R_Wrist",
			"a prop id matching no template is an ordinary inline attach");
	} else {
		Check(false, "test.props.inline loads with its attach");
	}

	// A bare single-scene file gets the registry too — unlike `roles`, `props` is not envelope-only.
	if (const auto* attach = firstAttach("test.props.bare")) {
		Check(attach->propAttachment.node == "R_AnimObject1" && attach->propAttachment.scale == 0.5f,
			"a bare single-scene file resolves its own top-level 'props'");
	} else {
		Check(false, "test.props.bare loads with its attach");
	}

	// -- `atFrame` positions: authored frames, resolved as clip-local seconds ---------------------
	if (const auto s = reg.Find("test.reg.frames")) {
		if (s->nodes.size() == 1) {
			const auto& node = s->nodes[0];
			Check(node.cues.size() == 3 && node.cues[0].pos == TrackPos::kFraction &&
				node.cues[0].frame == 0.0f && node.cues[1].frame == 24.0f,
				"atFrame cues parse as frame-carrying fraction positions");
			Check(node.cues[2].frame < 0.0f && node.cues[2].fraction == 0.5f,
				"a fractional cue alongside them keeps frame unset");
			Check(OSF::Registry::TrackSeconds(node.cues[1]) == 24.0f / OSF::Registry::kFrameRate,
				"a frame resolves to clip-local seconds at the scene frame rate");
			Check(OSF::Registry::TrackSeconds(node.cues[2]) < 0.0f,
				"a fractional entry reports no absolute seconds");
			// The fraction axis (browser + inspection scrub) needs a duration; 1.6 s = 48 frames.
			Check(OSF::Registry::TrackFraction(node.cues[1], 1.6f) == 0.5f,
				"a frame maps onto the fraction axis against the clip length");
			Check(OSF::Registry::TrackFraction(node.cues[1], 0.0f) == 1.0f &&
				OSF::Registry::TrackFraction(node.cues[0], 0.0f) == 0.0f,
				"with no clip length only frame 0 has a knowable place on the fraction axis");
			// Reachability mirrors Scene::Advance's [prev, next) window: a frame at or past the
			// clip end never fires, so the inspector/catalog must not present it as an end mark.
			Check(OSF::Registry::TrackFires(node.cues[1], 1.6f) &&
				!OSF::Registry::TrackFires(node.cues[1], 0.8f) &&
				!OSF::Registry::TrackFires(node.cues[1], 0.5f),
				"a frame fires only strictly inside the clip");
			Check(OSF::Registry::TrackFires(node.cues[1], 0.0f),
				"an unknown clip length is not judged unreachable");
			Check(OSF::Registry::TrackFires(node.cues[2], 0.4f),
				"fraction entries are never judged by TrackFires");
			Check(node.actions.size() == 1 && node.actions[0].frame == 15.0f,
				"atFrame parses on the action lane");
			Check(node.cameras.size() == 1 && node.cameras[0].frame == 9.0f,
				"atFrame parses on the camera lane");
			Check(node.sounds.size() == 3 && node.sounds[0].frame == 12.0f &&
				node.sounds[1].frame == 30.0f &&
				node.sounds[2].frame < 0.0f && node.sounds[2].fraction == 0.9f,
				"an atFrame sound ladder expands in frames, and a per-hit `at` opts back into fractions");
		} else {
			Check(false, "test.reg.frames desugars to one node");
		}
	} else {
		Check(false, "test.reg.frames loads");
	}

	// -- compiled route contract: document defaults and stage-local cue/action lanes -------------
	if (const auto s = reg.Find("test.route.compiled.single")) {
		Check(s->unlisted && s->inPlace, "compiled route inherits unlisted and inPlace document defaults");
		Check(!s->playerControl.enabled,
			"document-level playerControl:false revokes input for a scene that omits its own");
		Check(s->priority == 3 && s->weight == 7,
			"scene inherits the file-level matchmaking priority and weight");
		Check(s->tags.size() == 2 && s->tagSet.contains("route") && s->tagSet.contains("fixture"),
			"scene with no tags of its own inherits the file-level tags");
		Check(s->roles.size() == 1 && s->roles[0].name == "player" && s->roles[0].mask == "upperBody",
			"compiled route inherits the upperBody role mask");
		Check(s->nodes.size() == 1 && s->linearStages.size() == 1 && s->entry == "#s0",
			"single-stage route desugars to one linear node");
		if (s->nodes.size() == 1) {
			const auto& node = s->nodes[0];
			const auto* stage = node.stages.size() == 1 ? &node.stages[0] : nullptr;
			Check(node.cameras.empty(), "camera:none suppresses the document camera default");
			Check(node.loopMode == LoopMode::kCount && node.loopCount == 1,
				"compiled single stage keeps loops:1");
			Check(stage && stage->loops == 1 && stage->clips.size() == 1 &&
				stage->clips[0].file == "Fixture/Route/single.glb",
				"compiled single-stage clip and loop policy parse");
			Check(stage && node.cues.size() == 3 && stage->cues.size() == 3,
				"compiled stage cue lane is retained and forwarded to its node");
			if (node.cues.size() == 3) {
				Check(node.cues[0].id == "route.start" && node.cues[0].pos == TrackPos::kFraction &&
					node.cues[0].fraction == 0.0f &&
					node.cues[1].id == "route.source.hidden" && node.cues[1].fraction == 0.375f &&
					node.cues[2].id == "route.end" && node.cues[2].pos == TrackPos::kEnd,
					"compiled numeric and named cue positions parse");
			}
			Check(stage && node.actions.size() == 2 && stage->actions.size() == 2,
				"compiled stage action lane is retained and forwarded to its node");
			if (node.actions.size() == 2) {
				const auto& attach = node.actions[0];
				const auto& destroy = node.actions[1];
				Check(attach.kind == ActionKind::kPropAttach && attach.pos == TrackPos::kFraction &&
					attach.fraction == 0.25f && attach.role == "player" && attach.prop == "helmet",
					"compiled prop attach identity and timing parse");
				Check(attach.propSource.kind == OSF::Props::SourceKind::kEquippedArmor &&
					attach.propSource.keywords.size() == 2 &&
					attach.propSource.keywords[0] == "ArmorTypeSpacesuitHelmet" &&
					attach.propSource.keywords[1] == "ArmorTypeHelmet",
					"compiled equipped-armor source parses");
				Check(attach.propAttachment.node == "R_AnimObject1" &&
					attach.propAttachment.position[0] == 1.25f &&
					attach.propAttachment.position[1] == -2.5f &&
					attach.propAttachment.position[2] == 3.75f &&
					attach.propAttachment.rotation[0] == 10.0f &&
					attach.propAttachment.rotation[1] == -20.0f &&
					attach.propAttachment.rotation[2] == 30.0f &&
					attach.propAttachment.scale == 0.875f,
					"compiled prop node, three-component transforms, and scale parse");
				Check(destroy.kind == ActionKind::kPropDestroy && destroy.pos == TrackPos::kFraction &&
					destroy.fraction == 0.75f && destroy.prop == "helmet",
					"compiled prop destroy identity and timing parse");
				Check(OSF::Scene::InspectionPropsAt(node.actions, 0.249f, false).empty(),
					"inspection prop state is absent before attach");
				const auto attached = OSF::Scene::InspectionPropsAt(node.actions, 0.25f, false);
				Check(attached.size() == 1 && attached[0].prop == "helmet" &&
					attached[0].propAttachment.node == "R_AnimObject1",
					"inspection prop state includes attach at its exact mark");
				Check(OSF::Scene::InspectionPropsAt(node.actions, 0.749f, false).size() == 1,
					"inspection prop state persists between marks");
				Check(OSF::Scene::InspectionPropsAt(node.actions, 0.75f, false).empty(),
					"inspection prop state removes the prop at its destroy mark");
				auto reverseAuthored = node.actions;
				std::reverse(reverseAuthored.begin(), reverseAuthored.end());
				Check(OSF::Scene::InspectionPropsAt(reverseAuthored, 0.5f, false).size() == 1 &&
					OSF::Scene::InspectionPropsAt(reverseAuthored, 0.75f, false).empty(),
					"inspection prop state follows mark time rather than authored array order");
			}
			const auto plan = reg.BuildNodePlan(s, node, 1);
			Check(plan && !plan->anchored && plan->masks.size() == 1 && plan->masks[0] == "upperBody",
				"compiled route builds an in-place upperBody scene plan");
		}
	} else {
		Check(false, "test.route.compiled.single loads");
	}
	if (const auto s = reg.Find("test.route.compiled.two-stage")) {
		Check(s->unlisted && s->inPlace && s->roles.size() == 1 && s->roles[0].mask == "upperBody",
			"two-stage route inherits document policy and mask defaults");
		Check(s->playerControl.enabled &&
			(s->playerControl.capabilities & static_cast<std::uint32_t>(OSF::Input::Capability::kSpeed)) == 0 &&
			(s->playerControl.capabilities & static_cast<std::uint32_t>(OSF::Input::Capability::kAdvance)) != 0,
			"scene playerControl re-enables over the document default and narrows capabilities");
		// File tags come first in author order, the scene's own append, and "Fixture" collapses into
		// the inherited "fixture" (matchmaking is case-insensitive).
		Check(s->tags.size() == 3 && s->tags[0] == "route" && s->tags[1] == "fixture" &&
			s->tags[2] == "two-stage" && s->tagSet.size() == 3,
			"scene tags union with the file-level tags, de-duplicated case-insensitively");
		Check(s->priority == 9 && s->weight == 7,
			"scene priority overrides the file default while weight still inherits");
		Check(s->nodes.size() == 2 && s->linearStages.size() == 2 &&
			s->linearStages[0] == "#s0" && s->linearStages[1] == "#s1",
			"two-stage route desugars in authored order");
		const auto* first = s->FindNode("#s0");
		const auto* second = s->FindNode("#s1");
		Check(first && second, "two-stage route exposes both synthetic nodes");
		if (first && second) {
			Check(first->loopMode == LoopMode::kCount && first->loopCount == 1 &&
				second->loopMode == LoopMode::kCount && second->loopCount == 1,
				"both compiled route stages preserve loops:1");
			Check(first->stages.size() == 1 && first->stages[0].clips[0].file == "Fixture/Route/handoff.glb" &&
				second->stages.size() == 1 && second->stages[0].clips[0].file == "Fixture/Route/hold.glb",
				"two-stage route clip order parses");
			Check(!first->stages[0].clips[0].mask.has_value() && second->stages[0].clips[0].mask == "rightArm",
				"a clip-local mask applies only to its stage");
			Check(first->actions.size() == 1 && first->actions[0].kind == ActionKind::kPropAttach,
				"carry-forward stage omits a destroy before the next stage");
			Check(second->cues.size() == 1 && second->stages[0].cues.size() == 1 &&
				second->cues[0].id == "route.station.entered" && second->cues[0].pos == TrackPos::kEnter,
				"stage-two enter cue is retained and forwarded");
			Check(second->actions.size() == 2 && second->stages[0].actions.size() == 2 &&
				second->actions[0].kind == ActionKind::kPropAttach &&
				second->actions[0].pos == TrackPos::kFraction && second->actions[0].fraction == 0.0f,
				"stage-two numeric-zero prop reattach is retained and forwarded");
			const auto firstPlan = reg.BuildNodePlan(s, *first, 1);
			const auto secondPlan = reg.BuildNodePlan(s, *second, 1);
			Check(firstPlan && firstPlan->stages[0].masks[0] == "upperBody" &&
				secondPlan && secondPlan->stages[0].masks[0] == "rightArm",
				"a stage mask overrides and then falls back to the scene role mask");
		}
	} else {
		Check(false, "test.route.compiled.two-stage loads");
	}
	// -- `hold`: a stage frozen on one frame ------------------------------------------------------
	if (const auto s = reg.Find("test.hold.frame")) {
		Check(s->nodes.size() == 2 && s->nodes[0].stages[0].hold < 0.0f && s->nodes[1].stages[0].hold == 1.0f,
			"'hold': true freezes a stage on its last frame and leaves its neighbours playing");
		Check(s->nodes.size() == 2 && s->nodes[1].loopMode == LoopMode::kHold &&
			s->nodes[1].stages[0].loops == 0,
			"an untimed frozen stage holds until advanced instead of taking the play-once default");
		Check(s->nodes.size() == 2 && s->nodes[1].cues.size() == 1,
			"a frozen stage still carries its track lanes");
	} else {
		Check(false, "test.hold.frame loads");
	}
	if (const auto s = reg.Find("test.hold.timed")) {
		Check(s->nodes.size() == 1 && s->nodes[0].stages[0].hold == 0.25f && s->nodes[0].timerSec == 2.0f,
			"a fractional hold parks mid-clip and keeps its timer as the way out");
	} else {
		Check(false, "test.hold.timed loads");
	}
	Check(!reg.Find("test.hold.loops"), "'hold' with 'loops' is rejected — a frozen clip never loops");
	Check(!reg.Find("test.hold.range"), "a hold outside [0, 1] is rejected");
	{
		const auto errors = reg.LoadErrors();
		CheckError(errors, "cannot combine with 'loops'", "the hold/loops rejection explains itself");
		CheckError(errors, "clip position in [0, 1]", "an out-of-range hold rejection names the valid range");
	}

	// The frozen clock itself: entering a hold stage parks the clip and fires the marks at or before
	// the hold pose exactly once, and only a timer moves on from there.
	{
		using OSF::Animation::Scene;
		using OSF::Animation::TimedMark;
		const auto mark = [](float a_fraction, const char* a_token) {
			TimedMark m;
			m.fraction = a_fraction;
			m.token = a_token;
			return m;
		};
		Scene scene;
		Scene::StageData frozen;
		frozen.hold = 1.0f;
		frozen.duration = 4.0f;
		frozen.timer = 1.0f;
		frozen.marks.push_back(mark(0.0f, "enter"));
		frozen.marks.push_back(mark(0.5f, "midway"));
		Scene::StageData after;
		after.duration = 4.0f;
		scene.stages = { frozen, after };
		Check(scene.SetStage(0), "a frozen stage can be entered");

		const int token = 0;
		auto tick = scene.Advance(&token, 0.5f);
		Check(tick.stage == 0 && tick.time > 3.9f && tick.time < 4.0f,
			"a hold of 1.0 parks on the last representable pose, never frame zero");
		std::vector<OSF::Animation::FiredMark> fired;
		scene.DrainFiredMarks(fired);
		Check(fired.size() == 2, "marks at or before the hold pose fire once on entry");

		tick = scene.Advance(&token, 0.4f);
		scene.DrainFiredMarks(fired);
		Check(tick.stage == 0 && tick.time > 3.9f && tick.time < 4.0f && fired.empty(),
			"the frozen clock does not advance and does not re-fire its marks");

		tick = scene.Advance(&token, 0.2f);
		Check(tick.stage == 1 && tick.time == 0.0f, "only the timer leaves a frozen stage");
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
			auto validStagePolicy = *plan;
			validStagePolicy.stages[0].poseModes = { PoseMode::kOverride, PoseMode::kAdditive, PoseMode::kOverride };
			validStagePolicy.stages[0].poseWeights = { 1.0f, 0.5f, 0.25f };
			Check(OSF::Animation::HasValidRolePolicyShape(validStagePolicy, 3),
				"scene-plan validation accepts complete stage-local pose policies");
			validStagePolicy.stages[0].poseWeights.pop_back();
			Check(!OSF::Animation::HasValidRolePolicyShape(validStagePolicy, 3),
				"scene-plan validation rejects a stage-local pose-weight count mismatch");
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
	Check(!reg.Find("test.err.sound-emitter"), "unknown sound emitter rejects its scene");
	Check(!reg.Find("test.route.compiled.error.mask"), "compiled route rejects an unknown mask");
	Check(!reg.Find("test.route.compiled.error.scale"), "compiled route rejects a zero prop scale");
	Check(!reg.Find("test.route.compiled.error.source"), "compiled route rejects a malformed prop source");
	Check(!reg.Find("test.route.compiled.error.at"), "compiled route rejects an out-of-range cue position");
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
	Check(!reg.Find("test.props.err.use"), "a 'use' naming no prop template rejects its scene");
	Check(!reg.Find("test.props.err.use-type"), "a non-string 'use' rejects its scene, never silently ignored");
	Check(!reg.Find("test.props.err.typo"), "a prop id matching neither a template nor an inline source rejects its scene");
	Check(!reg.Find("test.props.err.use-elsewhere"), "'use' on a non-attach action rejects its scene");
	Check(!reg.Find("test.props.badtype.never"), "a non-object 'props' rejects its file");
	Check(!reg.Find("test.props.baddef.never"), "a malformed prop template rejects its file");
	if (const auto s = reg.Find("test.props.err.ok")) {
		Check(s->nodes.size() == 1 && s->nodes[0].actions.size() == 1 &&
			s->nodes[0].actions[0].propAttachment.node == "R_AnimObject1",
			"prop reference errors reject only their own scene");
	} else {
		Check(false, "test.props.err.ok loads");
	}
	const auto errors = reg.LoadErrors();
	for (const auto& e : errors) {
		std::cout << "  diag: " << e << '\n';
	}
	Check(errors.size() == 43, "exactly the forty-three expected diagnostics");
	CheckError(errors, "policy/arbitration key 'stripActors'", "route policy keys are rejected");
	CheckError(errors, "layer: 'mask'", "route animation masks are mandatory");
	CheckError(errors, "duplicate station id", "duplicate route station ids are rejected");
	CheckError(errors, "from/to must name declared stations", "route endpoints are validated");
	CheckError(errors, "commit: 'marker'", "route commit syntax is validated");
	CheckError(errors, "unknown lifetime", "route lifetimes are validated");
	CheckError(errors, "markers are instantaneous", "route markers reject misleading lifetime promises");
	CheckError(errors, "sounds are one-shot", "route sounds reject unsupported cancellation lifetimes");
	CheckError(errors, "policy/arbitration key 'claims'", "phase-two claims are rejected in schema v1");
	CheckError(errors, "duplicate route id", "duplicate route ids are rejected first-wins");
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
	CheckError(errors, "sound 'Sound/OSF/test.wem' has unknown 'emitter' value 'actor'",
		"unknown sound emitter diagnostic names the scene, node, cue, and value");
	CheckError(errors, "scene 'test.route.compiled.error.mask': role 'player': unknown 'mask' value 'torso'",
		"compiled-route mask diagnostic names the scene, role, and value");
	CheckError(errors, "action 'osf.prop.attach' scale must be finite and in (0,10]",
		"compiled-route scale diagnostic states the accepted range");
	CheckError(errors, "action 'osf.prop.attach' source.equippedArmor requires 'keyword'",
		"compiled-route source diagnostic identifies the malformed selector");
	CheckError(errors, "cue 'route.out-of-range' numeric 'at' must be in [0,1)",
		"compiled-route cue diagnostic states the fraction range");
	CheckError(errors, "cue 'half.frame' 'atFrame' must be a whole frame number >= 0",
		"fractional atFrame diagnostic states the whole-frame contract");
	CheckError(errors, "cue 'two.clocks' sets both 'at' and 'atFrame'",
		"a lane entry cannot carry both position keys");
	CheckError(errors, "a sound ladder hit sets both 'at' and 'atFrame'",
		"a ladder per-hit object cannot carry both position keys either");
	CheckError(errors, "'fixture_malformed_def.osf.json': roles registry entry 'bad'", "malformed-definition diagnostic");
	CheckError(errors, "'fixture_malformed_type.osf.json': file-level 'roles' must be an array", "registry type diagnostic");
	CheckError(errors, "'fixture_registry_template_errors.osf.json': scene 'test.terr.unknown': role reference 'nope'",
		"unknown object-override id diagnostic");
	CheckError(errors, "scene 'test.terr.empty': a role object's 'id' must be a non-empty string", "empty-id diagnostic");
	CheckError(errors, "scene 'test.terr.num': a role object's 'id' must be a non-empty string", "non-string-id diagnostic");
	CheckError(errors, "scene 'test.terr.dup': duplicate role name 'lead'", "duplicate explicit-name diagnostic");
	CheckError(errors, "action 'osf.prop.attach' prop template 'nope' is not defined in this file's top-level 'props' registry",
		"dangling prop-template diagnostic mirrors the roles-registry wording");
	CheckError(errors, "action 'osf.prop.attach' 'use' must be a non-empty string",
		"non-string 'use' diagnostic states the contract");
	CheckError(errors, "prop 'helmt' has no inline 'source' and no matching entry",
		"a typo'd prop id points at the registry instead of just 'requires source'");
	CheckError(errors, "'use' is only meaningful on 'osf.prop.attach'",
		"'use' outside an attach names the one action that accepts it");
	CheckError(errors, "'fixture_props_bad_type.osf.json': 'props' must be an object",
		"non-object props diagnostic rejects the file by name");
	CheckError(errors, "'fixture_props_bad_def.osf.json': props template 'helmet' has unknown key 'at'",
		"malformed-template diagnostic names the file, template, and offending key");
	CheckError(errors, "duplicate clipLibrary registration for",
		"duplicate clipLibrary diagnostic names the registered clip");
	CheckError(errors, "'folder' contains an empty segment",
		"invalid clipLibrary folder diagnostic names the path problem");
	Check(!OSF::Animation::NormalizePoseWeight(std::numeric_limits<double>::quiet_NaN()).has_value(),
		"non-finite poseWeight normalization rejects NaN");

	// -- scrub-only scene clock ---------------------------------------------------------------
	{
		OSF::Animation::Scene scene;
		OSF::Animation::Scene::StageData stage;
		stage.duration = 2.0f;
		stage.marks.push_back({ .fraction = 0.25f, .lane = 1, .token = "quarter" });
		scene.stages.push_back(std::move(stage));
		Check(scene.SetStage(0), "scene seek fixture selects its stage");
		int owner = 0;
		(void)scene.Advance(&owner, 0.75f);
		std::vector<OSF::Animation::FiredMark> fired;
		scene.DrainFiredMarks(fired);
		Check(fired.size() == 1 && fired[0].token == "quarter", "scene fires a crossed mark before seeking");
		Check(scene.Seek(1.25f), "scene seek accepts a finite in-range time");
		scene.DrainFiredMarks(fired);
		Check(fired.empty(), "scene seek itself does not fire marks");
		(void)scene.Advance(&owner, 0.1f);
		scene.DrainFiredMarks(fired);
		Check(fired.empty(), "resuming after a seek does not replay consumed marks");
		Check(scene.Seek(2.0f) && scene.GetPlaybackSnapshot().time < 2.0f,
			"scene seek clamps clip end to the final representable pose");
		Check(!scene.Seek(std::numeric_limits<float>::quiet_NaN()), "scene seek rejects non-finite time");
	}

	// -- absolute (frame-authored) marks vs. fractional ones ------------------------------------
	{
		// The same mark pair on two clips of different lengths: the frame mark (0.5 s = frame 15 at
		// 30 fps) holds its wall position, the fractional one slides with the clip.
		const auto firedTimes = [](float a_duration) {
			OSF::Animation::Scene scene;
			OSF::Animation::Scene::StageData stage;
			stage.duration = a_duration;
			stage.marks.push_back({ .seconds = 0.5f, .lane = 0, .token = "frame" });
			stage.marks.push_back({ .fraction = 0.5f, .lane = 1, .token = "half" });
			scene.stages.push_back(std::move(stage));
			(void)scene.SetStage(0);
			int owner = 0;
			std::vector<std::pair<std::string, float>> hits;
			std::vector<OSF::Animation::FiredMark> fired;
			for (float t = 0.0f; t < a_duration; t += 0.05f) {
				(void)scene.Advance(&owner, 0.05f);
				scene.DrainFiredMarks(fired);
				for (const auto& f : fired) {
					hits.emplace_back(f.token, t + 0.05f);
				}
			}
			return hits;
		};
		const auto shortClip = firedTimes(1.0f);   // frame 15 = halfway
		const auto longClip = firedTimes(2.0f);    // frame 15 = a quarter in
		const auto at = [](const auto& a_hits, std::string_view a_token) {
			for (const auto& h : a_hits) {
				if (h.first == a_token) {
					return h.second;
				}
			}
			return -1.0f;
		};
		Check(std::abs(at(shortClip, "frame") - 0.5f) < 0.051f &&
			std::abs(at(longClip, "frame") - 0.5f) < 0.051f,
			"an absolute mark fires at its authored clip time whatever the clip's length");
		Check(std::abs(at(shortClip, "half") - 0.5f) < 0.051f &&
			std::abs(at(longClip, "half") - 1.0f) < 0.051f,
			"a fractional mark still scales with the clip's length");

		// A frame past the clip end has nowhere to land and never fires (documented, not an error).
		OSF::Animation::Scene late;
		OSF::Animation::Scene::StageData stage;
		stage.duration = 1.0f;
		stage.marks.push_back({ .seconds = 4.0f, .everyLoop = true, .lane = 0, .token = "past-end" });
		late.stages.push_back(std::move(stage));
		(void)late.SetStage(0);
		int owner = 0;
		std::size_t lateHits = 0;
		std::vector<OSF::Animation::FiredMark> fired;
		for (int i = 0; i < 40; i++) {  // two full loops of the clip
			(void)late.Advance(&owner, 0.05f);
			late.DrainFiredMarks(fired);
			lateHits += fired.size();
		}
		Check(lateHits == 0, "a mark past the clip end never fires");
	}

	// -- per-file import records (the browser's IMPORTS listing) --------------------------------
	{
		const auto files = reg.FileStats();
		Check(files.size() == 22, "one import record per discovered *.osf.json");

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
		Check(accepted == 35, "per-file scene counts sum to the authored total");
		Check(owned == errors.size(), "every load problem is attributed to exactly one file");
		Check(sorted, "import records are sorted by path");
		Check(pathsRelative, "import record paths are Data/OSF-relative and forward-slashed");
		const auto* overlayRoute = find("fixture_route.osf.json");
		Check(overlayRoute && overlayRoute->declaredRoutes == 1 && overlayRoute->routes == 1 &&
			overlayRoute->rejectedRoutes == 0, "clean route files report accepted/declared/rejected route counts");
		const auto* overlayErrors = find("fixture_route_errors.osf.json");
		Check(overlayErrors && overlayErrors->declaredRoutes == 11 && overlayErrors->routes == 1 &&
			overlayErrors->rejectedRoutes == 10 && !overlayErrors->Rejected(),
			"route-local failures preserve valid sibling routes and report exact rejection counts");

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
		Check(malformed && !malformed->problems.empty() &&
			malformed->problems[0].code == "file-invalid" && !malformed->problems[0].hint.empty(),
			"rejected files carry structured repair guidance");
		Check(malformed && malformed->Rejected(), "a file that contributed nothing is flagged rejected");

		// Partial file: some scenes in, thirteen rejected, so it is NOT "rejected".
		const auto* partial = find("fixture_registry_errors.osf.json");
		Check(partial && partial->scenes == 1 && partial->errors == 13,
			"a partially-loaded file reports both its scenes and its rejected ones");
		Check(partial && partial->declaredScenes == 14 && partial->rejectedScenes == 13,
			"partial files report exact authored and rejected scene counts");
		Check(partial && std::ranges::all_of(partial->problems, [](const auto& a_problem) {
			return !a_problem.code.empty() && !a_problem.hint.empty(); }), "scene diagnostics carry structured codes and repair hints");
		Check(partial && !partial->Rejected(), "a file that loaded something is not flagged rejected");

		const auto* route = find("fixture_route_compiled.osf.json");
		Check(route && route->scenes == 2 && route->declaredScenes == 2 && route->rejectedScenes == 0,
			"compiled route fixture reports both accepted scenes");
		Check(route && route->unlisted == 2 && route->anchored == 0 && route->nodes == 3 && route->stages == 3,
			"compiled route fixture reports document policy and stage totals");
		Check(route && route->cues == 5 && route->actions == 5 && route->cameras == 0,
			"compiled route fixture reports its cue/action lanes and camera:none default");

		const auto* routeErrors = find("fixture_route_compiled_errors.osf.json");
		Check(routeErrors && routeErrors->scenes == 0 && routeErrors->declaredScenes == 4 &&
			routeErrors->rejectedScenes == 4 && routeErrors->errors == 4,
			"compiled route error fixture reports four independent scene rejections");
		Check(routeErrors && routeErrors->Rejected(),
			"compiled route error fixture is flagged rejected after contributing no scenes");

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

	return Finish("Scene registry");
}
