#include "OSFScript.h"

#include "Animation/GraphManager.h"
#include "Audio/SoundService.h"
#include "Matchmaking/Matchmaker.h"
#include "Registry/SceneRegistry.h"
#include "Registry/SoundRegistry.h"
#include "Scene/SceneEventRelay.h"
#include "Scene/SceneRuntime.h"
#include "Serialization/AFImport.h"
#include "Serialization/GLTFImport.h"
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
			std::int32_t       stripMode = -1;       // tri-state override: -1 inherit, 0 off, 1 on
			std::int32_t       lockPlayerMode = -1;  // tri-state override
			std::int32_t       fadeMode = -1;        // tri-state override
			float              loopScale = 1.0f;     // multiply loop-driven stage loop counts (1.0 = none)
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
			if (const auto* v = member("FadeMode"); v && v->is<std::int32_t>()) {
				out.fadeMode = RE::BSScript::get<std::int32_t>(*v);
			}
			if (const auto* v = member("LoopScale"); v && v->is<float>()) {
				out.loopScale = RE::BSScript::get<float>(*v);
			}
			return out;
		}

		// A SceneRuntime world-anchor from resolved options (unset when no Anchor).
		// Heading < 0 uses the ref's own facing; otherwise the explicit degrees, converted to radians.
		Scene::SceneRuntime::AnchorOverride MakeAnchor(const SceneOpts& a_opts)
		{
			Scene::SceneRuntime::AnchorOverride anchor{};
			if (a_opts.anchor) {
				anchor.set = true;
				anchor.pos = a_opts.anchor->data.location;
				anchor.heading = (a_opts.headingDeg < 0.0f) ? a_opts.anchor->data.angle.z : (a_opts.headingDeg * Util::kDegToRadF);
			}
			return anchor;
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
			over.fade = triState(a_opts.fadeMode);
			float ls = a_opts.loopScale;
			if (!(ls > 0.0f)) {  // false for <=0 AND for NaN -> no-op
				ls = 1.0f;
			} else if (ls > kLoopScaleMax) {
				ls = kLoopScaleMax;
			}
			over.loopScale = ls;
			return over;
		}

		// Start a matchmade candidate using its resolved binding (Matchmaking::Pick already chose the
		// slot->actor order, so we never re-bind here). Binds by declaration order = the reordered actors.
		int32_t StartCandidate(const Matchmaking::Candidate& a_pick, const std::vector<RE::Actor*>& a_actors,
			const Scene::SceneRuntime::StartOverrides& a_over)
		{
			std::vector<RE::Actor*> ordered(a_pick.order.size());
			for (size_t slot = 0; slot < a_pick.order.size(); slot++) {
				ordered[slot] = a_actors[a_pick.order[slot]];
			}
			return Scene::SceneRuntime::GetSingleton().StartFromDef(a_pick.id, ordered, a_over);
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
			if (!Registry::SceneRegistry::GetSingleton().Find(sid)) {
				REX::DEBUG("[Papyrus] StartScene: no scene '{}'", sid);
				return 0;
			}
			const auto opts = ReadSceneOptions(a_opts);
			const auto anchor = MakeAnchor(opts);
			const auto over = MakeOverrides(opts);
			auto& rt = Scene::SceneRuntime::GetSingleton();
			if (anchor.set) {
				return rt.StartFromDefAt(sid, a_actors, anchor.pos, anchor.heading, over);  // anchored at the ref
			}
			return rt.StartFromDef(sid, a_actors, over);
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
			std::vector<std::string> roles;
			roles.reserve(a_roles.size());
			for (const auto& r : a_roles) {
				roles.emplace_back(r.c_str());
			}
			return Scene::SceneRuntime::GetSingleton().StartFromDefRoles(sid, a_actors, roles);
		}

		// Shared body of the StartSceneByTags* natives: validate the actor list, matchmake a_query across both registries (priority tier + weighted-random),
		// and start the picked candidate with its matchmade binding. a_logTag is the native name for the warn/info lines.
		// Returns  the scene handle (0 = no actors / null actor / no match / start failed).
		int32_t StartMatched(const std::vector<RE::Actor*>& a_actors, const Matchmaking::TagQuery& a_query,
			const Scene::SceneRuntime::StartOverrides& a_over, const char* a_logTag)
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
			auto pick = Matchmaking::Pick(a_actors, a_query);
			if (!pick) {
				REX::DEBUG("[Papyrus] {}: no matching animation found for the given tags/actors", a_logTag);
				return 0;
			}
			const int32_t handle = StartCandidate(*pick, a_actors, a_over);
			if (handle) {
				REX::INFO("[Papyrus] {}: playing '{}' handle {:#010x}", a_logTag, pick->id, handle);
			} else {
				REX::WARN("[Papyrus] {}: could not start matched scene '{}'", a_logTag, pick->id);
			}
			return handle;
		}

		// Anchored counterpart of StartCandidate: start the matchmade pick world-anchored at (a_pos,
		// a_heading) instead of co-locating the actors at actor[0], so it can sit on a bed/furniture/marker.
		int32_t StartCandidateAt(const Matchmaking::Candidate& a_pick, const std::vector<RE::Actor*>& a_actors, const RE::NiPoint3& a_pos, float a_heading,
			const Scene::SceneRuntime::StartOverrides& a_over)
		{
			std::vector<RE::Actor*> ordered(a_pick.order.size());
			for (size_t slot = 0; slot < a_pick.order.size(); slot++) {
				ordered[slot] = a_actors[a_pick.order[slot]];
			}
			return Scene::SceneRuntime::GetSingleton().StartFromDefAt(a_pick.id, ordered, a_pos, a_heading, a_over);
		}

		// Anchored counterpart of StartMatched: matchmake a_query, then start the pick anchored at (a_pos, a_heading).
		// Shared body of the StartSceneByTags*At natives.
		int32_t StartMatchedAt(const std::vector<RE::Actor*>& a_actors, const Matchmaking::TagQuery& a_query,
			const RE::NiPoint3& a_pos, float a_heading, const Scene::SceneRuntime::StartOverrides& a_over, const char* a_logTag)
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
			auto pick = Matchmaking::Pick(a_actors, a_query);
			if (!pick) {
				REX::DEBUG("[Papyrus] {}: no matching animation found for the given tags/actors", a_logTag);
				return 0;
			}
			const int32_t handle = StartCandidateAt(*pick, a_actors, a_pos, a_heading, a_over);
			if (handle) {
				REX::INFO("[Papyrus] {}: playing '{}' handle {:#010x} (anchored)", a_logTag, pick->id, handle);
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
			const auto anchor = MakeAnchor(opts);
			const auto over = MakeOverrides(opts);
			if (anchor.set) {
				return StartMatchedAt(a_actors, q, anchor.pos, anchor.heading, over, "OSF.StartSceneByTags");
			}
			return StartMatched(a_actors, q, over, "OSF.StartSceneByTags");
		}

		// Boolean-query form of StartSceneByTags: all-of / any-of / none-of tag sets, otherwise identical (filter-aware matchmaking across both registries, priority + weighted pick).
		// akOpts.Anchor world-anchors the matchmade scene.
		int32_t StartSceneByTagsQuery(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_allOf, std::vector<RE::BSFixedString> a_anyOf,
			std::vector<RE::BSFixedString> a_noneOf, SceneOptionsArg a_opts)
		{
			Matchmaking::TagQuery q{ ToTags(a_allOf), ToTags(a_anyOf), ToTags(a_noneOf) };
			const auto opts = ReadSceneOptions(a_opts);
			const auto anchor = MakeAnchor(opts);
			const auto over = MakeOverrides(opts);
			if (anchor.set) {
				return StartMatchedAt(a_actors, q, anchor.pos, anchor.heading, over, "OSF.StartSceneByTagsQuery");
			}
			return StartMatched(a_actors, q, over, "OSF.StartSceneByTagsQuery");
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

		int32_t StartSceneRolesEx(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			RE::BSFixedString a_id, std::vector<RE::BSFixedString> a_roles, SceneOptionsArg a_opts)
		{
			if (a_actors.empty()) {
				REX::DEBUG("[Papyrus] StartSceneRolesEx: no actors given");
				return 0;
			}
			const auto opts = ReadSceneOptions(a_opts);
			const int32_t handle = Scene::SceneRuntime::GetSingleton().StartFromDefRoles(
				a_id.c_str(), a_actors, ToStrings(a_roles), MakeAnchor(opts), MakeOverrides(opts));
			if (handle && opts.stage > 0) {
				Scene::SceneRuntime::GetSingleton().SetStage(handle, opts.stage);
			}
			return handle;
		}

		bool PlaySequence(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor,
			std::vector<RE::BSFixedString> a_files, std::vector<int32_t> a_loops, std::vector<float> a_blends, bool a_loopWhole)
		{
			return Animation::GraphManager::GetSingleton().PlaySequence(a_actor, ToStrings(a_files), a_loops, a_blends, a_loopWhole);
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
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneRoles", &StartSceneRoles, true, false);

		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "StartSceneFiles", &StartSceneFiles, true, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "StartSceneRolesEx", &StartSceneRolesEx, true, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "StartSceneStages", &StartSceneStages, true, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "PlaySequence", &PlaySequence, true, false);
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
		REX::INFO("[Papyrus] registered natives on script '{}'", SCRIPT_NAME);
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
