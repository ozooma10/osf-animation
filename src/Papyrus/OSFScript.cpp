#include "OSFScript.h"

#include "API/UIBridge.h"  // PushCatalogUpdate (duration rescan on ReloadPacks)
#include "Animation/GraphManager.h"
#include "Animation/Scene.h"  // ParticipantPlacement + PlacementToWorld (anchor-offset composition)
#include "Audio/SoundService.h"
#include "Camera/CameraService.h"
#include "Input/InputService.h"
#include "Matchmaking/Matchmaker.h"
#include "Player/PlayerControlService.h"
#include "Registry/SceneRegistry.h"
#include "Registry/SoundRegistry.h"
#include "Scene/AnchorResolve.h"
#include "Scene/SceneEventRelay.h"
#include "Scene/SceneRuntime.h"
#include "Serialization/AFImport.h"
#include "Serialization/ClipDurations.h"
#include "Serialization/GLTFImport.h"
#include "UI/HudMessage.h"
#include "UI/PortraitService.h"  // CaptureNow (portrait acceptance-test native)
#include "Util/Math.h"
#include "Util/StringUtil.h"

#include "RE/B/BSScriptUtil.h"
#include "RE/S/Struct.h"
#include "RE/S/StructTypeInfo.h"

#include <algorithm>
#include <filesystem>
#include <format>

namespace OSF::Papyrus
{
	namespace
	{
		using OSFVM = RE::BSScript::IVirtualMachine;

		std::vector<std::string> ToStrings(const std::vector<RE::BSFixedString>& a_in)
		{
			std::vector<std::string> out;
			out.reserve(a_in.size());
			for (const auto& s : a_in) {
				out.emplace_back(s.c_str());
			}
			return out;
		}

		// Tag-query variant: lowercases each tag (satisfying Matchmaker's "query is
		// already lowercased" precondition) and drops empty entries from unfilled
		// Papyrus array slots that would otherwise poison an allOf match.
		std::vector<std::string> ToTags(const std::vector<RE::BSFixedString>& a_in)
		{
			std::vector<std::string> out;
			out.reserve(a_in.size());
			for (const auto& s : a_in) {
				auto t = Util::ToLower(s.c_str());
				if (!t.empty()) {
					out.push_back(std::move(t));
				}
			}
			return out;
		}

		// --- OSFTypes:SceneOptions (the trailing `SceneOptions akOpts = None` on the Start* natives) ----
		// A struct parameter arrives as this wrapper; the `= None` default makes it optional.
		using SceneOptionsArg = std::optional<RE::BSScript::structure_wrapper<"OSFTypes", "SceneOptions">>;

		// SceneOption defaults. Keep in sync with OSFTypes.psc SceneOptions.
		struct SceneOpts
		{
			RE::TESObjectREFR* anchor = nullptr;
			float              headingDeg = -1.0f;
			std::int32_t       stage = 0;
			float              speed = 1.0f;
			float              blendIn = 0.4f;
			std::int32_t       stripMode = -1;          // tri-state override: -1 inherit, 0 off, 1 on
			std::int32_t       lockPlayerMode = -1;     // tri-state override
			std::int32_t       playerControlMode = -1;  // tri-state override of the director-input grant (OFF = no advance/end)
			std::int32_t       fadeMode = -1;           // tri-state override
			std::string        camera;                  // entry camera state override ("" = inherit the scene's)
			float              loopScale = 1.0f;        // multiply loop-driven stage loop counts (1.0 = none)
		};

		// Read an OSF:SceneOptions: pull the raw struct proxy and map member name -> slot by ITERATING varNameIndexMap, rather than trusting structure_wrapper::find / varNameIndexMap.find
		SceneOpts ReadSceneOptions(const SceneOptionsArg& a_opts)
		{
			SceneOpts out;
			if (!a_opts) {
				return out;
			}
			const RE::BSTSmartPointer<RE::BSScript::Struct> proxy =
				RE::BSScript::detail::wrapper_accessor::get_proxy(*a_opts);
			if (!proxy || !proxy->type) {
				return out;
			}

			std::unordered_map<std::string, std::uint32_t> index;
			for (const auto& kv : proxy->type->varNameIndexMap) {
				index[Util::ToLower(kv.key.c_str())] = kv.value;
			}
			const auto count = proxy->type->variables.size();
			const auto member = [&](const char* a_name) -> const RE::BSScript::Variable* {
				const auto it = index.find(Util::ToLower(a_name));
				return (it != index.end() && it->second < count) ? &proxy->variables[it->second] : nullptr;
			};

			if (const auto* v = member("Anchor"); v && v->is<RE::BSScript::Object>() && RE::BSScript::get<RE::BSScript::Object>(*v)) {
				out.anchor = RE::BSScript::UnpackVariable<RE::TESObjectREFR>(*v);
			}
			if (const auto* v = member("HeadingDeg"); v && v->is<float>()) {
				out.headingDeg = RE::BSScript::get<float>(*v);
			}
			if (const auto* v = member("Stage"); v && v->is<std::int32_t>()) {
				out.stage = RE::BSScript::get<std::int32_t>(*v);
			}
			if (const auto* v = member("Speed"); v && v->is<float>()) {
				out.speed = RE::BSScript::get<float>(*v);
			}
			if (const auto* v = member("BlendIn"); v && v->is<float>()) {
				out.blendIn = RE::BSScript::get<float>(*v);
			}
			if (const auto* v = member("StripMode"); v && v->is<std::int32_t>()) {
				out.stripMode = RE::BSScript::get<std::int32_t>(*v);
			}
			if (const auto* v = member("LockPlayerMode"); v && v->is<std::int32_t>()) {
				out.lockPlayerMode = RE::BSScript::get<std::int32_t>(*v);
			}
			if (const auto* v = member("PlayerControlMode"); v && v->is<std::int32_t>()) {
				out.playerControlMode = RE::BSScript::get<std::int32_t>(*v);
			}
			if (const auto* v = member("FadeMode"); v && v->is<std::int32_t>()) {
				out.fadeMode = RE::BSScript::get<std::int32_t>(*v);
			}
			if (const auto* v = member("Camera"); v && v->is<RE::BSFixedString>()) {
				out.camera = RE::BSScript::get<RE::BSFixedString>(*v).c_str();
			}
			if (const auto* v = member("LoopScale"); v && v->is<float>()) {
				out.loopScale = RE::BSScript::get<float>(*v);
			}
			return out;
		}

		// Optional explicit heading (radians) from SceneOptions: HeadingDeg < 0 => use the ref's own facing.
		std::optional<float> OptHeadingRad(const SceneOpts& a_opts)
		{
			return (a_opts.headingDeg < 0.0f) ? std::nullopt : std::optional<float>(a_opts.headingDeg * Util::kDegToRadF);
		}

		// A SceneRuntime world-anchor from resolved options (unset when no Anchor).
		Scene::SceneRuntime::AnchorOverride MakeAnchor(const SceneOpts& a_opts)
		{
			return Scene::MakeAnchorAt(a_opts.anchor, OptHeadingRad(a_opts));
		}

		// Resolve + ENFORCE a start's anchor requirement
		std::optional<Scene::SceneRuntime::AnchorOverride> ResolveSceneAnchor(const std::string& a_sceneId, const SceneOpts& a_opts)
		{
			return Scene::ResolveSceneAnchor(a_sceneId, a_opts.anchor, OptHeadingRad(a_opts), /*a_emitHud*/ true);
		}

		// Upper bound for LoopScale
		constexpr float kLoopScaleMax = 20.0f;

		// SceneRuntime per-start overrides from resolved options. Tri-state ints map to optional<bool> (1 = on, 0 = off, anything else incl. -1 = inherit the scene's pack default). 
		// LoopScale is sanitized: <=0 or NaN -> 1.0 (no scaling); inf / overshoot -> clamped to kLoopScaleMax.
		Scene::SceneRuntime::StartOverrides MakeOverrides(const SceneOpts& a_opts)
		{
			Scene::SceneRuntime::StartOverrides over{};
			const auto triState = [](std::int32_t a_v) -> std::optional<bool> {
				if (a_v == 1) {
					return true;
				}
				if (a_v == 0) {
					return false;
				}
				return std::nullopt;  // -1 and any out-of-range value = inherit
			};
			over.strip = triState(a_opts.stripMode);
			over.lockPlayer = triState(a_opts.lockPlayerMode);
			over.playerControl = triState(a_opts.playerControlMode);
			over.fade = triState(a_opts.fadeMode);
			if (!a_opts.camera.empty()) {
				over.camera = a_opts.camera;
			}
			float ls = a_opts.loopScale;
			if (!(ls > 0.0f)) {  // false for <=0 AND for NaN -> no-op
				ls = 1.0f;
			} else if (ls > kLoopScaleMax) {
				ls = kLoopScaleMax;
			}
			over.loopScale = ls;
			return over;
		}

		// SceneOptions.Stage (> 0) -> the linear-stage entry node for a def start, so the scene ENTERS on
		// that stage instead of playing its entry first and jumping. Returns nullopt on a hard failure
		// (stage out of range — the caller fails the start loudly); "" = enter at the scene's own entry
		// (stage 0, or warn-and-ignore when the scene has no linear stages).
		std::optional<std::string> ResolveStageEntryNode(const Registry::SceneDef& a_def, std::int32_t a_stage, const char* a_tag)
		{
			if (a_stage <= 0) {
				return std::string{};
			}
			if (a_def.linearStages.empty()) {
				REX::WARN("[Papyrus] {}: scene '{}' has no linear stages — SceneOptions.Stage {} ignored", a_tag, a_def.id, a_stage);
				return std::string{};
			}
			if (a_stage >= static_cast<std::int32_t>(a_def.linearStages.size())) {
				REX::WARN("[Papyrus] {}: stage {} out of range for scene '{}' ({} stages)", a_tag, a_stage, a_def.id, a_def.linearStages.size());
				UI::HudMessage::Error(std::format("scene '{}' has no stage {}", a_def.id, a_stage));
				return std::nullopt;
			}
			return a_def.linearStages[a_stage];
		}

		// Start a matchmade candidate using its resolved binding (Matchmaking::Pick already chose the slot->actor order, so we never re-bind here) at an already-resolved anchor.
		// Binds by declaration order = the reordered actors. anchor unset => co-located at actor[0]; set => world-anchored.
		int32_t StartCandidate(const Matchmaking::Candidate& a_pick, const std::vector<RE::Actor*>& a_actors,
			const Scene::SceneRuntime::AnchorOverride& a_anchor, const Scene::SceneRuntime::StartOverrides& a_over)
		{
			std::vector<RE::Actor*> ordered(a_pick.order.size());
			for (size_t slot = 0; slot < a_pick.order.size(); slot++) {
				ordered[slot] = a_actors[a_pick.order[slot]];
			}
			auto& rt = Scene::SceneRuntime::GetSingleton();
			if (a_anchor.set) {
				return rt.StartFromDefAt(a_pick.id, ordered, a_anchor.pos, a_anchor.heading, a_over);
			}
			return rt.StartFromDef(a_pick.id, ordered, a_over);
		}

		bool Play(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor, RE::BSFixedString a_file, RE::BSFixedString a_animId)
		{
			if (!a_actor) {
				REX::DEBUG("[Papyrus] Play: no actor given");
				return false;
			}
			if (!Animation::GraphManager::GetSingleton().PlayAnimation(a_actor, a_file.c_str(), a_animId.c_str())) {
				REX::DEBUG("[Papyrus] Play: could not find/play animation '{}' (id '{}')", a_file.c_str(), a_animId.c_str());
				return false;
			}
			return true;
		}

		bool Stop(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			if (!a_actor) {
				REX::DEBUG("[Papyrus] Stop: no actor given");
				return false;
			}
			return Animation::GraphManager::GetSingleton().StopAnimation(a_actor);
		}

		// Group the actors' independently-played solo graphs onto one clock. With abAnchor=true (default) it also
		// promotes them into a single anchored, co-located scene at actor[0]'s transform, so each clip's baked root
		// offset arranges the actors about one shared origin+heading (the NAF/SAF "play each solo, then sync" pattern).
		// abAnchor=false is the legacy clock-only merge (actors stay at their own world positions). Needs >= 2 actors.
		bool Sync(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors, bool a_anchor)
		{
			return Animation::GraphManager::GetSingleton().Sync(a_actors, a_anchor);
		}

		// Jump a linear scene (by handle) to a given stage. False on a non-linear graph, an out-of-range stage, or an invalid handle.
		bool SetSceneStage(OSFVM&, uint32_t, std::monostate, int32_t a_scene, int32_t a_stage)
		{
			return Scene::SceneRuntime::GetSingleton().SetStage(a_scene, a_stage);
		}

		// Actor convenience: jump the live GraphManager scene a_actor is in to a stage (0-based).
		// Reaches any GraphManager scene including a PlaySequence solo sequence (no handle).
		bool SetSceneStageForActor(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor, int32_t a_stage)
		{
			if (!a_actor) {
				REX::DEBUG("[Papyrus] SetSceneStageForActor: no actor given");
				return false;
			}
			return Animation::GraphManager::GetSingleton().SetSceneStage(a_actor, a_stage);
		}

		// Rescans Data/OSF/**/*.osf.json (dev convenience: edit a scene, reload, replay without
		// restarting the game). Returns the number of scenes loaded. Also drops the clip caches so
		// edited animation files re-import. (Name kept for the existing Papyrus binding.)
		int32_t ReloadPacks(OSFVM&, uint32_t, std::monostate)
		{
			Serialization::GLTFImport::ClearCache();
			Serialization::AFImport::ClearCache();
			REX::DEBUG("[Papyrus] ReloadPacks: clip cache cleared");
			auto& registry = Registry::SceneRegistry::GetSingleton();
			registry.LoadAll();
			Registry::SoundRegistry::GetSingleton().LoadAll();
			// Re-probe clip durations: edited files fail the size/mtime check and get fresh values,
			// then the catalog re-pushes so the browser's time estimates follow the edit loop.
			Serialization::ClipDurations::ScanSceneClipsAsync(&API::PushCatalogUpdate);
			return static_cast<int32_t>(registry.Size());
		}

		// Current stage of a LINEAR scene (by handle), or -1 (non-linear graph / invalid handle).
		int32_t GetSceneStage(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			return Scene::SceneRuntime::GetSingleton().GetStage(a_scene);
		}

		// Actor convenience: current stage of the live GraphManager scene a_actor is in, or -1.
		// Reaches any GraphManager scene including a PlaySequence solo sequence (no handle).
		int32_t GetSceneStageForActor(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			return Animation::GraphManager::GetSingleton().GetSceneStage(a_actor);
		}

		bool IsPlaying(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			return Animation::GraphManager::GetSingleton().IsPlaying(a_actor);
		}

		// Current source file path playing on a_actor (as the caller passed it), or "" if the actor has no live graph.
		RE::BSFixedString GetCurrentAnimation(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			if (!a_actor) {
				return "";
			}
			return RE::BSFixedString(Animation::GraphManager::GetSingleton().GetCurrentAnimation(a_actor).c_str());
		}

		// Per-graph playback speed (1.0 = authored; 0 = freeze; clamped 0..100).
		// For a scene participant this sets the shared scene clock speed.
		bool SetSpeed(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor, float a_speed)
		{
			if (!a_actor) {
				REX::DEBUG("[Papyrus] SetSpeed: no actor given");
				return false;
			}
			return Animation::GraphManager::GetSingleton().SetSpeed(a_actor, a_speed);
		}

		float GetSpeed(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			if (!a_actor) {
				return 0.0f;
			}
			return Animation::GraphManager::GetSingleton().GetSpeed(a_actor);
		}

		// Pin akActor's solo graph to a WORLD point + heading (degrees). 
		// aiRootMode: 0 pin / 1 additive / 2 follow. 
		// Also moves the capsule there. Refused for scene participants (their placement is scene-driven).
		bool SetAnchor(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor,
			float a_x, float a_y, float a_z, float a_headingDeg, int32_t a_rootMode)
		{
			if (!a_actor) {
				REX::DEBUG("[Papyrus] SetAnchor: no actor given");
				return false;
			}
			return Animation::GraphManager::GetSingleton().SetAnchor(a_actor, a_x, a_y, a_z, a_headingDeg, a_rootMode);
		}

		// Releases akActor's anchor — the graph returns to "follow" (rides the actor's live transform). No-op if no anchor was set.
		bool ClearAnchor(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			if (!a_actor) {
				return false;
			}
			return Animation::GraphManager::GetSingleton().ClearAnchor(a_actor);
		}

		// Stop a live scene by its handle (from a Start* call). False if the handle is invalid/ended. Fires NODE_EXIT + SCENE_END to registered callbacks.
		bool StopScene(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			return Scene::SceneRuntime::GetSingleton().Stop(a_scene);
		}

		// Actor convenience: stop the live scene a_actor participates in. Tries the SceneRuntime handle first (fires NODE_EXIT + SCENE_END, invalidates it);
		// falls back to a raw GraphManager scene with no handle (a PlaySequence solo sequence). False if in neither.
		bool StopSceneForActor(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			if (!a_actor) {
				REX::DEBUG("[Papyrus] StopSceneForActor: no actor given");
				return false;
			}
			if (Scene::SceneRuntime::GetSingleton().StopForActor(a_actor)) {
				return true;
			}
			return Animation::GraphManager::GetSingleton().StopScene(a_actor);
		}

		// Framework semver "major.minor.patch" (string so it can't be misread).
		RE::BSFixedString GetVersion(OSFVM&, uint32_t, std::monostate)
		{
			const auto v = SFSE::GetPluginVersion();
			return RE::BSFixedString(std::format("{}.{}.{}", v.major(), v.minor(), v.patch()));
		}

		// True once OSF is loaded and initialized (playback hooks installed).
		bool IsReady(OSFVM&, uint32_t, std::monostate)
		{
			return Animation::GraphManager::GetSingleton().HooksInstalled();
		}

		// Open the in-game scene browser (the OSF UI "osf" view), exactly as F10 would.
		// Backs the Data Slate item so the browser is discoverable without knowing the hotkey.
		// False if OSF UI is absent or too old to open a menu from native code.
		bool OpenBrowser(OSFVM&, uint32_t, std::monostate)
		{
			return API::OpenBrowser();
		}

		// Capture akActor's face to the portrait cache, rendered on the LIVE inventory
		// paperdoll. REQUIRES the inventory/apparel (paperdoll) screen to be open and
		// rendering — it hijacks that doll, swaps in akActor's face, shoots a PNG, then
		// restores the player. Returns false if the capture couldn't start (paperdoll not
		// open, engine gate failed, or the actor has no NPC base). The acceptance-test /
		// manual entry point; the async result lands in the browser's portrait cache.
		bool CapturePortrait(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			if (!a_actor) {
				REX::DEBUG("[Papyrus] CapturePortrait: no actor given");
				return false;
			}
			return UI::Portraits::CaptureNow(a_actor);
		}

		// Start a scene by id, returning an opaque scene HANDLE (0 = failed).
		// Routes through SceneRuntime so callbacks fire and the handle drives GetSceneId/Node/StopScene/etc.
		// akOpts.Anchor world-anchors the scene at a ref instead of co-locating at actor[0].
		// For named-role binding use StartSceneRoles.
		int32_t StartScene(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors, RE::BSFixedString a_id,
			SceneOptionsArg a_opts)
		{
			if (a_actors.empty()) {
				REX::DEBUG("[Papyrus] StartScene: no actors given");
				return 0;
			}
			const std::string sid = a_id.c_str();
			const auto* def = Registry::SceneRegistry::GetSingleton().Find(sid);
			if (!def) {
				REX::DEBUG("[Papyrus] StartScene: no scene '{}'", sid);
				return 0;
			}
			const auto opts = ReadSceneOptions(a_opts);
			// SceneOptions.Stage: enter the scene directly on this linear stage instead of its entry (0 = the scene's own entry).
			// Lets a browser open a sequence on one animation. Out of range is a caller error → fail loudly; a non-linear graph has no stages → warn and ignore.
			const auto entryNode = ResolveStageEntryNode(*def, opts.stage, "OSF.StartScene");
			if (!entryNode) {
				return 0;
			}
			const auto anchor = ResolveSceneAnchor(sid, opts);  // enforces an anchor-bound scene's furniture requirement
			if (!anchor) {
				return 0;  // anchor-bound scene with no / incompatible anchor ref (logged in ResolveSceneAnchor)
			}
			const auto over = MakeOverrides(opts);
			auto& rt = Scene::SceneRuntime::GetSingleton();
			if (anchor->set) {
				return rt.StartFromDefAt(sid, a_actors, anchor->pos, anchor->heading, over, *entryNode);  // anchored at the ref
			}
			return rt.StartFromDef(sid, a_actors, over, *entryNode);
		}

		// Start a scene binding actors to NAMED roles: asRoles[i] is the role for akActors[i]
		// (equal lengths). Returns the handle (0 = no such scene / validation fail). Its own native rather
		// than a SceneOptions field because Papyrus structs cannot carry the role array.
		int32_t StartSceneRoles(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			RE::BSFixedString a_id, std::vector<RE::BSFixedString> a_roles)
		{
			if (a_actors.empty()) {
				REX::DEBUG("[Papyrus] StartSceneRoles: no actors given");
				return 0;
			}
			const std::string sid = a_id.c_str();
			// StartSceneRoles carries no SceneOptions, so it can't supply the anchor an anchor-bound scene
			// needs. Reject early with a clear pointer to StartScene rather than failing the placement later.
			if (const auto* def = Registry::SceneRegistry::GetSingleton().Find(sid); def && def->RequiresAnchor()) {
				REX::WARN("[Papyrus] StartSceneRoles: scene '{}' is anchor-bound — use StartScene with SceneOptions.Anchor", sid);
				UI::HudMessage::Error(std::format("scene '{}' needs a furniture anchor (use StartScene)", sid));
				return 0;
			}
			std::vector<std::string> roles;
			roles.reserve(a_roles.size());
			for (const auto& r : a_roles) {
				roles.emplace_back(r.c_str());
			}
			return Scene::SceneRuntime::GetSingleton().StartFromDefRoles(sid, a_actors, roles);
		}

		// Shared body of the StartSceneByTags* / StartSceneAtAnchor natives: validate the actor list, matchmake a_query across the scene registry (priority tier + weighted-random) with anchor filtering (a_mode + a_opts.anchor), 
		// ENFORCE the picked scene's anchor requirement (ResolveSceneAnchor), and start the pick with its matchmade binding at the resolved anchor. 
		// Returns the scene handle (0 = no actors / null actor / no match / anchor rejected / start failed).
		int32_t StartMatched(const std::vector<RE::Actor*>& a_actors, const Matchmaking::TagQuery& a_query, const SceneOpts& a_opts, const Scene::SceneRuntime::StartOverrides& a_over, const char* a_logTag, Matchmaking::AnchorMode a_mode = Matchmaking::AnchorMode::kAllow)
		{
			if (a_actors.empty()) {
				REX::DEBUG("[Papyrus] {}: no actors given", a_logTag);
				return 0;
			}
			for (auto* actor : a_actors) {
				if (!actor) {
					REX::DEBUG("[Papyrus] {}: null actor in list", a_logTag);
					return 0;
				}
			}
			auto pick = Matchmaking::Pick(a_actors, a_query, a_opts.anchor, a_mode);
			if (!pick) {
				REX::DEBUG("[Papyrus] {}: no matching animation found for the given tags/actors", a_logTag);
				UI::HudMessage::Error("no matching animation for the given tags/actors");
				return 0;
			}
			const auto anchor = ResolveSceneAnchor(pick->id, a_opts);
			if (!anchor) {
				return 0;  // anchor-bound pick without a compatible anchor (logged in ResolveSceneAnchor)
			}
			const int32_t handle = StartCandidate(*pick, a_actors, *anchor, a_over);
			if (handle) {
				REX::INFO("[Papyrus] {}: playing '{}' handle {:#010x}{}", a_logTag, pick->id, handle, anchor->set ? " (anchored)" : "");
			} else {
				REX::WARN("[Papyrus] {}: could not start matched scene '{}'", a_logTag, pick->id);
				UI::HudMessage::Error(std::format("could not start scene '{}'", pick->id));
			}
			return handle;
		}

		// Matchmake by tags + role/gender fit across BOTH registries (composed scene defs + packs), pick by priority tier + weighted-random, and start it with the matchmade binding.
		// Returns the scene handle (0 = no match / start failed); GetSceneId(handle) recovers the chosen id.
		// akOpts.Anchor world-anchors the matchmade scene at a ref (furniture/bed/marker) instead of co-locating at actor[0].
		int32_t StartSceneByTags(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors, std::vector<RE::BSFixedString> a_tags, SceneOptionsArg a_opts)
		{
			Matchmaking::TagQuery q;
			q.allOf = ToTags(a_tags);
			const auto opts = ReadSceneOptions(a_opts);
			const auto over = MakeOverrides(opts);
			return StartMatched(a_actors, q, opts, over, "OSF.StartSceneByTags");
		}

		// Boolean-query form of StartSceneByTags: all-of / any-of / none-of tag sets, otherwise identical (filter-aware matchmaking across both registries, priority + weighted pick).
		// akOpts.Anchor world-anchors the matchmade scene.
		int32_t StartSceneByTagsQuery(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_allOf, std::vector<RE::BSFixedString> a_anyOf,
			std::vector<RE::BSFixedString> a_noneOf, SceneOptionsArg a_opts)
		{
			Matchmaking::TagQuery q{ ToTags(a_allOf), ToTags(a_anyOf), ToTags(a_noneOf) };
			const auto opts = ReadSceneOptions(a_opts);
			const auto over = MakeOverrides(opts);
			return StartMatched(a_actors, q, opts, over, "OSF.StartSceneByTagsQuery");
		}

		// Anchor-FIRST matchmaking: given a furniture/object ref, start a scene BUILT for it. 
		// Restricts the pool to anchor-bound scenes whose keyword/base matcher akFurniture satisfies AND whose roles/tags/gender fit the actors,
		// asTags optionally narrows the pool (empty = any fit).
		// akOpts is read for overrides + heading; its Anchor field is ignored (akFurniture is the anchor).
		// Returns the scene handle (0 = no furniture / no actors / no fitting scene / start failed).
		int32_t StartSceneAtAnchor(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors, RE::TESObjectREFR* a_furniture, std::vector<RE::BSFixedString> a_tags, SceneOptionsArg a_opts)
		{
			if (a_actors.empty()) {
				REX::DEBUG("[Papyrus] StartSceneAtAnchor: no actors given");
				return 0;
			}
			if (!a_furniture) {
				REX::DEBUG("[Papyrus] StartSceneAtAnchor: no furniture given");
				UI::HudMessage::Error("no furniture given");
				return 0;
			}
			Matchmaking::TagQuery q;
			q.allOf = ToTags(a_tags);
			auto opts = ReadSceneOptions(a_opts);
			opts.anchor = a_furniture;  // the anchor-first ref drives BOTH the matchmaking filter and the compose
			const auto over = MakeOverrides(opts);
			return StartMatched(a_actors, q, opts, over, "OSF.StartSceneAtAnchor", Matchmaking::AnchorMode::kRequire);
		}

		// --- Scene-event callbacks (OSFTypes:SceneEvent payload) --------------------
		int32_t RegisterSceneCallback(OSFVM&, uint32_t, std::monostate, RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_fn, int32_t a_scene, int32_t a_eventMask)
		{
			if (!a_receiver.get()) {
				REX::DEBUG("[Papyrus] RegisterSceneCallback: null receiver");
				return 0;
			}
			return Scene::SceneEventRelay::GetSingleton().Register(a_receiver, a_fn.c_str(), a_scene, a_eventMask);
		}

		// Instance-free variant for Papyrus script libraries: dispatch to the GLOBAL function
		// asScript.asFn(OSFTypes:SceneEvent). Same scene filter / event mask / token semantics as
		// RegisterSceneCallback; use UnregisterSceneCallback to drop the token.
		int32_t RegisterSceneCallbackStatic(OSFVM&, uint32_t, std::monostate, RE::BSFixedString a_script, RE::BSFixedString a_fn, int32_t a_scene, int32_t a_eventMask)
		{
			if (a_script.empty()) {
				REX::DEBUG("[Papyrus] RegisterSceneCallbackStatic: empty script name");
				return 0;
			}
			return Scene::SceneEventRelay::GetSingleton().RegisterStatic(a_script.c_str(), a_fn.c_str(), a_scene, a_eventMask);
		}

		bool UnregisterSceneCallback(OSFVM&, uint32_t, std::monostate, int32_t a_token)
		{
			return Scene::SceneEventRelay::GetSingleton().Unregister(a_token);
		}

		// --- Navigation (def-backed scenes) -----------------------------------------
		// Handle-based, mirroring SetSceneStage/GetSceneStage: the SceneRuntime resolves the
		// handle and returns contract sentinels (false / 0 / "") for an invalid or non-def scene.

		// Take the current node's DEFAULT advance edge (or end the scene if it targets "$end").
		// False if the handle is invalid or the node has no default advance edge.
		bool AdvanceScene(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			return Scene::SceneRuntime::GetSingleton().Advance(a_scene);
		}

		// Take the current node's branchable advance edge whose id == asEdgeId.
		// False if the handle is invalid or the current node has no such edge.
		bool NavigateScene(OSFVM&, uint32_t, std::monostate, int32_t a_scene, RE::BSFixedString a_edgeId)
		{
			return Scene::SceneRuntime::GetSingleton().Navigate(a_scene, a_edgeId.c_str());
		}

		// Number of branchable (advance) edges on the current node, for building a choice menu. 0 if invalid.
		int32_t GetSceneEdgeCount(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			return Scene::SceneRuntime::GetSingleton().EdgeCount(a_scene);
		}

		// Id of the a_index-th branchable edge (0 .. count-1) of the current node. "" if out of range/invalid.
		RE::BSFixedString GetSceneEdgeId(OSFVM&, uint32_t, std::monostate, int32_t a_scene, int32_t a_index)
		{
			return RE::BSFixedString(Scene::SceneRuntime::GetSingleton().EdgeId(a_scene, a_index).c_str());
		}

		// Resolved label (labelKey or literal) of the a_index-th branchable edge of the current node. "" if out of range/invalid.
		RE::BSFixedString GetSceneEdgeLabel(OSFVM&, uint32_t, std::monostate, int32_t a_scene, int32_t a_index)
		{
			return RE::BSFixedString(Scene::SceneRuntime::GetSingleton().EdgeLabel(a_scene, a_index).c_str());
		}

		// Participant roster of the live scene named by the handle, in scene-internal
		// (role-declaration) order. Empty array if the handle is invalid/ended.
		std::vector<RE::Actor*> GetSceneParticipants(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			return Scene::SceneRuntime::GetSingleton().GetParticipants(a_scene);
		}

		bool IsGltfPath(std::string_view a_path)
		{
			const auto ext = Util::ToLower(std::filesystem::path{ std::string(a_path) }.extension().string());
			return ext == ".glb" || ext == ".gltf";
		}

		std::pair<std::string, std::string> SplitRuntimeClipSpec(std::string a_spec)
		{
			const auto pos = a_spec.rfind(':');
			if (pos == std::string::npos || pos + 1 >= a_spec.size()) {
				return { std::move(a_spec), {} };
			}
			std::string pathPart = a_spec.substr(0, pos);
			if (!IsGltfPath(pathPart)) {
				return { std::move(a_spec), {} };
			}
			std::string animId = a_spec.substr(pos + 1);
			return { std::move(pathPart), std::move(animId) };
		}

		Animation::ScenePlan::Stage MakeStageFromFiles(const std::vector<RE::BSFixedString>& a_files, std::size_t a_begin, std::size_t a_count)
		{
			Animation::ScenePlan::Stage stage;
			stage.files.reserve(a_count);
			stage.animIds.reserve(a_count);
			for (std::size_t i = 0; i < a_count; i++) {
				auto [file, animId] = SplitRuntimeClipSpec(a_files[a_begin + i].c_str());
				stage.files.push_back(std::move(file));
				stage.animIds.push_back(std::move(animId));
			}
			return stage;
		}

		bool PlacementArrayLenOk(std::size_t a_len, std::size_t a_actorCount, std::size_t a_totalCount)
		{
			return a_len == 0 || a_len == a_actorCount || a_len == a_totalCount;
		}

		bool PlacementArraysValid(const char* a_tag, std::size_t a_actorCount, std::size_t a_totalCount,
			const std::vector<float>& a_x, const std::vector<float>& a_y, const std::vector<float>& a_z, const std::vector<float>& a_headingDeg)
		{
			if (PlacementArrayLenOk(a_x.size(), a_actorCount, a_totalCount) &&
				PlacementArrayLenOk(a_y.size(), a_actorCount, a_totalCount) &&
				PlacementArrayLenOk(a_z.size(), a_actorCount, a_totalCount) &&
				PlacementArrayLenOk(a_headingDeg.size(), a_actorCount, a_totalCount)) {
				return true;
			}
			REX::DEBUG("[Papyrus] {}: placement arrays must be empty, actor-count ({}), or stage-major file-count ({}) (x/y/z/heading = {}/{}/{}/{})",
				a_tag, a_actorCount, a_totalCount, a_x.size(), a_y.size(), a_z.size(), a_headingDeg.size());
			return false;
		}

		float PlacementValue(const std::vector<float>& a_values, std::size_t a_stage, std::size_t a_actor, std::size_t a_actorCount)
		{
			if (a_values.empty()) {
				return 0.0f;
			}
			const std::size_t index = (a_values.size() == a_actorCount) ? a_actor : (a_stage * a_actorCount + a_actor);
			return a_values[index];
		}

		std::vector<Animation::ParticipantPlacement> MakePlacements(std::size_t a_actorCount, std::size_t a_stage,
			const std::vector<float>& a_x, const std::vector<float>& a_y, const std::vector<float>& a_z, const std::vector<float>& a_headingDeg)
		{
			std::vector<Animation::ParticipantPlacement> placements;
			placements.reserve(a_actorCount);
			for (std::size_t i = 0; i < a_actorCount; i++) {
				Animation::ParticipantPlacement p;
				p.x = PlacementValue(a_x, a_stage, i, a_actorCount);
				p.y = PlacementValue(a_y, a_stage, i, a_actorCount);
				p.z = PlacementValue(a_z, a_stage, i, a_actorCount);
				p.heading = PlacementValue(a_headingDeg, a_stage, i, a_actorCount) * Util::kDegToRadF;
				placements.push_back(p);
			}
			return placements;
		}

		int32_t StartSceneFiles(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_files, SceneOptionsArg a_opts)
		{
			if (a_actors.empty() || a_actors.size() != a_files.size()) {
				REX::DEBUG("[Papyrus] StartSceneFiles: actor/file count mismatch ({}/{})", a_actors.size(), a_files.size());
				return 0;
			}
			const auto opts = ReadSceneOptions(a_opts);
			Animation::ScenePlan plan;
			plan.stages.push_back(MakeStageFromFiles(a_files, 0, a_files.size()));
			plan.stages[0].loops = 0;
			plan.speed = opts.speed;
			plan.blendIn = opts.blendIn;
			return Scene::SceneRuntime::GetSingleton().StartFromPlan(a_actors, std::move(plan), 0, MakeAnchor(opts), MakeOverrides(opts));
		}

		int32_t StartSceneFilesPlaced(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_files, std::vector<float> a_x, std::vector<float> a_y, std::vector<float> a_z,
			std::vector<float> a_headingDeg, SceneOptionsArg a_opts)
		{
			if (a_actors.empty() || a_actors.size() != a_files.size()) {
				REX::DEBUG("[Papyrus] StartSceneFilesPlaced: actor/file count mismatch ({}/{})", a_actors.size(), a_files.size());
				return 0;
			}
			if (!PlacementArraysValid("StartSceneFilesPlaced", a_actors.size(), a_files.size(), a_x, a_y, a_z, a_headingDeg)) {
				return 0;
			}
			const auto opts = ReadSceneOptions(a_opts);
			Animation::ScenePlan plan;
			auto stage = MakeStageFromFiles(a_files, 0, a_files.size());
			stage.placements = MakePlacements(a_actors.size(), 0, a_x, a_y, a_z, a_headingDeg);
			stage.loops = 0;
			plan.stages.push_back(std::move(stage));
			plan.speed = opts.speed;
			plan.blendIn = opts.blendIn;
			return Scene::SceneRuntime::GetSingleton().StartFromPlan(a_actors, std::move(plan), 0, MakeAnchor(opts), MakeOverrides(opts));
		}

		int32_t StartSceneStages(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_files, std::vector<float> a_timers, std::vector<int32_t> a_loops,
			std::vector<float> a_blends, SceneOptionsArg a_opts)
		{
			if (a_actors.empty() || a_files.empty() || (a_files.size() % a_actors.size()) != 0) {
				REX::DEBUG("[Papyrus] StartSceneStages: files must be stage-major and divisible by actor count ({}/{})", a_files.size(), a_actors.size());
				return 0;
			}
			const std::size_t stageCount = a_files.size() / a_actors.size();
			const auto validLen = [stageCount](std::size_t n) { return n == 0 || n == stageCount; };
			if (!validLen(a_timers.size()) || !validLen(a_loops.size()) || !validLen(a_blends.size())) {
				REX::DEBUG("[Papyrus] StartSceneStages: timers/loops/blends must be empty or length {}", stageCount);
				return 0;
			}
			const auto opts = ReadSceneOptions(a_opts);
			Animation::ScenePlan plan;
			plan.speed = opts.speed;
			plan.blendIn = opts.blendIn;
			plan.stages.reserve(stageCount);
			const bool timingGiven = !a_timers.empty() || !a_loops.empty();
			for (std::size_t s = 0; s < stageCount; s++) {
				auto stage = MakeStageFromFiles(a_files, s * a_actors.size(), a_actors.size());
				stage.timer = a_timers.empty() ? 0.0f : a_timers[s];
				stage.loops = a_loops.empty() ? (timingGiven ? 0 : 1) : a_loops[s];
				stage.blendIn = a_blends.empty() ? opts.blendIn : a_blends[s];
				plan.stages.push_back(std::move(stage));
			}
			return Scene::SceneRuntime::GetSingleton().StartFromPlan(a_actors, std::move(plan), opts.stage, MakeAnchor(opts), MakeOverrides(opts));
		}

		int32_t StartSceneStagesPlaced(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_files, std::vector<float> a_timers, std::vector<int32_t> a_loops,
			std::vector<float> a_blends, std::vector<float> a_x, std::vector<float> a_y, std::vector<float> a_z,
			std::vector<float> a_headingDeg, SceneOptionsArg a_opts)
		{
			if (a_actors.empty() || a_files.empty() || (a_files.size() % a_actors.size()) != 0) {
				REX::DEBUG("[Papyrus] StartSceneStagesPlaced: files must be stage-major and divisible by actor count ({}/{})", a_files.size(), a_actors.size());
				return 0;
			}
			const std::size_t stageCount = a_files.size() / a_actors.size();
			const auto validLen = [stageCount](std::size_t n) { return n == 0 || n == stageCount; };
			if (!validLen(a_timers.size()) || !validLen(a_loops.size()) || !validLen(a_blends.size())) {
				REX::DEBUG("[Papyrus] StartSceneStagesPlaced: timers/loops/blends must be empty or length {}", stageCount);
				return 0;
			}
			if (!PlacementArraysValid("StartSceneStagesPlaced", a_actors.size(), a_files.size(), a_x, a_y, a_z, a_headingDeg)) {
				return 0;
			}
			const auto opts = ReadSceneOptions(a_opts);
			Animation::ScenePlan plan;
			plan.speed = opts.speed;
			plan.blendIn = opts.blendIn;
			plan.stages.reserve(stageCount);
			const bool timingGiven = !a_timers.empty() || !a_loops.empty();
			for (std::size_t s = 0; s < stageCount; s++) {
				auto stage = MakeStageFromFiles(a_files, s * a_actors.size(), a_actors.size());
				stage.placements = MakePlacements(a_actors.size(), s, a_x, a_y, a_z, a_headingDeg);
				stage.timer = a_timers.empty() ? 0.0f : a_timers[s];
				stage.loops = a_loops.empty() ? (timingGiven ? 0 : 1) : a_loops[s];
				stage.blendIn = a_blends.empty() ? opts.blendIn : a_blends[s];
				plan.stages.push_back(std::move(stage));
			}
			return Scene::SceneRuntime::GetSingleton().StartFromPlan(a_actors, std::move(plan), opts.stage, MakeAnchor(opts), MakeOverrides(opts));
		}

		int32_t StartSceneRolesEx(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			RE::BSFixedString a_id, std::vector<RE::BSFixedString> a_roles, SceneOptionsArg a_opts)
		{
			if (a_actors.empty()) {
				REX::DEBUG("[Papyrus] StartSceneRolesEx: no actors given");
				return 0;
			}
			const auto opts = ReadSceneOptions(a_opts);
			// SceneOptions.Stage enters directly on the stage node (same semantics as StartScene) instead of
			// the old post-start SetStage jump, which played the entry node first (visible pop, double load).
			std::string entryNode;
			if (const auto* def = Registry::SceneRegistry::GetSingleton().Find(a_id.c_str())) {
				auto resolved = ResolveStageEntryNode(*def, opts.stage, "OSF.StartSceneRolesEx");
				if (!resolved) {
					return 0;
				}
				entryNode = std::move(*resolved);
			}
			return Scene::SceneRuntime::GetSingleton().StartFromDefRoles(
				a_id.c_str(), a_actors, ToStrings(a_roles), MakeAnchor(opts), MakeOverrides(opts), entryNode);
		}

		bool PlaySequence(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor,
			std::vector<RE::BSFixedString> a_files, std::vector<int32_t> a_loops, std::vector<float> a_blends, bool a_loopWhole)
		{
			std::vector<std::string> files;
			std::vector<std::string> animIds;
			files.reserve(a_files.size());
			animIds.reserve(a_files.size());
			for (const auto& spec : a_files) {
				auto [file, animId] = SplitRuntimeClipSpec(spec.c_str());
				files.push_back(std::move(file));
				animIds.push_back(std::move(animId));
			}
			return Animation::GraphManager::GetSingleton().PlaySequence(a_actor, files, animIds, a_loops, a_blends, a_loopWhole);
		}

		bool HideEquipment(OSFVM&, uint32_t, std::monostate, int32_t a_scene, RE::Actor* a_actor, int32_t a_slotMask)
		{
			return Scene::SceneRuntime::GetSingleton().HideEquipment(a_scene, a_actor, static_cast<std::uint32_t>(a_slotMask));
		}

		bool RestoreEquipment(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			return Scene::SceneRuntime::GetSingleton().RestoreHiddenEquipment(a_scene);
		}

		int32_t StopAllForActors(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors)
		{
			std::vector<int32_t> handles;
			int32_t stopped = 0;
			for (auto* actor : a_actors) {
				if (!actor) {
					continue;
				}
				const int32_t h = Scene::SceneRuntime::GetSingleton().GetSceneForActor(actor);
				if (h && std::find(handles.begin(), handles.end(), h) == handles.end()) {
					handles.push_back(h);
				}
			}
			for (const int32_t h : handles) {
				if (Scene::SceneRuntime::GetSingleton().Stop(h)) {
					stopped++;
				}
			}
			for (auto* actor : a_actors) {
				if (actor && Animation::GraphManager::GetSingleton().IsPlaying(actor)) {
					if (Animation::GraphManager::GetSingleton().StopScene(actor) || Animation::GraphManager::GetSingleton().StopAnimation(actor)) {
						stopped++;
					}
				}
			}
			return stopped;
		}

		std::vector<RE::BSFixedString> GetSceneLoadErrors(OSFVM&, uint32_t, std::monostate)
		{
			std::vector<RE::BSFixedString> out;
			for (const auto& e : Registry::SceneRegistry::GetSingleton().LoadErrors()) {
				out.emplace_back(e.c_str());
			}
			return out;
		}

		std::vector<RE::BSFixedString> GetMissingClipRefs(OSFVM&, uint32_t, std::monostate)
		{
			std::vector<RE::BSFixedString> out;
			for (const auto& e : Registry::SceneRegistry::GetSingleton().MissingClipRefs()) {
				out.emplace_back(e.c_str());
			}
			return out;
		}

		// --- Compatibility-only natives (bound on OSFCompat) ---------------------------------------
		// The SAF shim's non-Scene Play+Sync path freezes the player via these standalone locks; the
		// core never applies them on its own (the scene runtime drives its own control/camera policy).

		// Standalone control lock: input-disable layer + AI-driven. false releases.
		// Also arms the input-hook "Activate -> talk to partner" redirect: legacy SAF/NAF mods (SnuSnu) drive
		// progression by talking to a participant, but OSF can't show the [E] prompt on a pinned, animation-
		// driven, player-overlapping NPC, so while this lock holds the Activate key is redirected (see InputService).
		void SetPlayerControlLock(OSFVM&, uint32_t, std::monostate, bool a_locked)
		{
			Player::PlayerControlService::GetSingleton().SetStandaloneLock(a_locked);
			Input::InputService::GetSingleton().SetCompatActivate(a_locked);
		}

		// The player's current SAF-compat scene partner (first other participant), or None. Lets the input-hook
		// redirect's Papyrus side (SAFScript.OnCompatActivate) find the actor to Activate.
		RE::Actor* GetScenePartner(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			return Animation::GraphManager::GetSingleton().GetScenePartner(a_actor);
		}

		// Standalone camera lock: force/hold third person (bounces on zoom-in). false restores the prior POV.
		void SetPlayerCameraLock(OSFVM&, uint32_t, std::monostate, bool a_locked)
		{
			Camera::CameraService::GetSingleton().SetStandaloneLock(a_locked);
		}

		// Engine crosshair target: the reference under the reticle / activate prompt.
		// Reads PlayerCharacter->commandTarget. Any ref kind, or null when the crosshair is on nothing.
		RE::TESObjectREFR* CrosshairTarget()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			return player ? player->commandTarget : nullptr;
		}

		// The raw engine crosshair reference, or None. Restores SAF's native crosshairRef the
		// pure-Papyrus shim had no way to read.
		RE::TESObjectREFR* GetCrosshairRef(OSFVM&, uint32_t, std::monostate)
		{
			return CrosshairTarget();
		}

		// The crosshair reference cast to Actor, or None when the crosshair is on nothing or a
		// non-actor ref. Backs the SAF shim's crosshair pickers (which otherwise approximate
		// selection with a pure-Papyrus heading-angle cone search).
		RE::Actor* GetCrosshairActor(OSFVM&, uint32_t, std::monostate)
		{
			auto* target = CrosshairTarget();
			return (target && target->IsActor()) ? static_cast<RE::Actor*>(target) : nullptr;
		}
	}

	void RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm)
	{
		// Force-load the OSFTypes script type so its SceneOptions + SceneEvent structs are registered
		// BEFORE we bind the struct-typed Start* natives AND before the relay's CreateStruct("OSFTypes#SceneEvent") at dispatch time.
		{
			RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> typesType;
			if (!a_vm->GetScriptObjectType(RE::BSFixedString(TYPES_SCRIPT_NAME.data()), typesType) || !typesType) {
				REX::ERROR("[Papyrus] could not preload '{}' type info — struct-typed natives "
						  "(StartScene/StartSceneByTags/StartSceneByTagsQuery) may fail to register and "
						  "scene-event callbacks may receive no payload", TYPES_SCRIPT_NAME);
			}
		}

		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneByTags", &StartSceneByTags, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartScene", &StartScene, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneByTagsQuery", &StartSceneByTagsQuery, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneAtAnchor", &StartSceneAtAnchor, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneRoles", &StartSceneRoles, true, false);

		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "StartSceneFiles", &StartSceneFiles, true, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "StartSceneFilesPlaced", &StartSceneFilesPlaced, true, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "StartSceneRolesEx", &StartSceneRolesEx, true, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "StartSceneStages", &StartSceneStages, true, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "StartSceneStagesPlaced", &StartSceneStagesPlaced, true, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "PlaySequence", &PlaySequence, true, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "HideEquipment", &HideEquipment, true, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "RestoreEquipment", &RestoreEquipment, true, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "StopAllForActors", &StopAllForActors, true, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "GetSceneLoadErrors", &GetSceneLoadErrors, true, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "GetMissingClipRefs", &GetMissingClipRefs, true, false);

		a_vm->BindNativeMethod(SCRIPT_NAME, "RegisterSceneCallback", &RegisterSceneCallback, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "RegisterSceneCallbackStatic", &RegisterSceneCallbackStatic, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "UnregisterSceneCallback", &UnregisterSceneCallback, true, false);

		a_vm->BindNativeMethod(SCRIPT_NAME, "AdvanceScene", &AdvanceScene, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "NavigateScene", &NavigateScene, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneEdgeCount", &GetSceneEdgeCount, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneEdgeId", &GetSceneEdgeId, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneEdgeLabel", &GetSceneEdgeLabel, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneParticipants", &GetSceneParticipants, true, false);

		a_vm->BindNativeMethod(SCRIPT_NAME, "IsPlaying", &IsPlaying, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "Play", &Play, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "Stop", &Stop, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "Sync", &Sync, true, false);

		a_vm->BindNativeMethod(SCRIPT_NAME, "SetSpeed", &SetSpeed, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSpeed", &GetSpeed, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "SetAnchor", &SetAnchor, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "ClearAnchor", &ClearAnchor, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetCurrentAnimation", &GetCurrentAnimation, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "SetSceneStage", &SetSceneStage, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneStage", &GetSceneStage, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "SetSceneStageForActor", &SetSceneStageForActor, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneStageForActor", &GetSceneStageForActor, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StopScene", &StopScene, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StopSceneForActor", &StopSceneForActor, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "ReloadPacks", &ReloadPacks, true, false);

		a_vm->BindNativeMethod(SCRIPT_NAME, "IsReady", &IsReady, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetVersion", &GetVersion, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "OpenBrowser", &OpenBrowser, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "CapturePortrait", &CapturePortrait, true, false);
		REX::INFO("[Papyrus] registered natives on script '{}'", SCRIPT_NAME);

		// Non-public compat natives (script 'OSFCompat'): the standalone player/camera lock the SAF
		// shim's primitive Play+Sync path uses, and the engine crosshair the pure-Papyrus shim can't
		// read. Kept off the public OSF surface; harmless when no SAF shim is installed to call them.
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "SetPlayerControlLock", &SetPlayerControlLock, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "SetPlayerCameraLock", &SetPlayerCameraLock, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "GetCrosshairRef", &GetCrosshairRef, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "GetCrosshairActor", &GetCrosshairActor, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "GetScenePartner", &GetScenePartner, true, false);
		REX::INFO("[Papyrus] registered compat natives on script '{}'", COMPAT_SCRIPT_NAME);

		// Wire the input-hook "Activate -> talk to partner" redirect to the Papyrus side: on the player's
		// Activate keypress during a SAF-compat scene, run SAFScript.OnCompatActivate() (it finds the partner
		// via GetScenePartner and calls partner.Activate(player), reproducing the legacy "Talk to" trigger).
		Input::InputService::GetSingleton().SetCompatActivateHandler([]() {
			auto* game = RE::GameVM::GetSingleton();
			auto* vm = game ? game->GetVM() : nullptr;
			if (!vm) {
				return;
			}
			const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> noCallback{};
			vm->DispatchStaticCall(
				"SAFScript", "OnCompatActivate",
				[](RE::BSScrapArray<RE::BSScript::Variable>& a_args) {
					a_args.clear();
					return true;
				},
				noCallback, 0);
		});
	}

	bool RegisterFunctions()
	{
		if (auto* gameVM = RE::GameVM::GetSingleton(); gameVM && gameVM->GetVM()) {
			RegisterFunctions(gameVM->GetVM());
			return true;
		}
		return false;
	}
}
