#include "OSFScript.h"

#include "Animation/GraphManager.h"
#include "Audio/SoundService.h"
#include "Matchmaking/Matchmaker.h"
#include "Registry/SceneRegistry.h"
#include "Scene/SceneEventRelay.h"
#include "Scene/SceneRuntime.h"
#include "Serialization/AFImport.h"
#include "Serialization/GLTFImport.h"
#include "Util/Math.h"
#include "Util/StringUtil.h"

#include "RE/B/BSScriptUtil.h"
#include "RE/S/Struct.h"
#include "RE/S/StructTypeInfo.h"

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

		// --- OSFTypes:SceneOptions (the trailing `SceneOptions akOpts = None` on the Start* natives) ----
		// A struct parameter arrives as this wrapper; the `= None` default makes it optional.
		using SceneOptionsArg = std::optional<RE::BSScript::structure_wrapper<"OSFTypes", "SceneOptions">>;

		// SceneOption defaults. Keep in sync with OSF.psc
		struct SceneOpts
		{
			RE::TESObjectREFR* anchor = nullptr;
			float              headingDeg = -1.0f;
			std::int32_t       stage = 0;
			float              speed = 1.0f;
			float              blendIn = 0.4f;
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

		// Start a matchmade candidate using its resolved binding (Matchmaking::Pick already chose the
		// slot->actor order, so we never re-bind here). Binds by declaration order = the reordered actors.
		int32_t StartCandidate(const Matchmaking::Candidate& a_pick, const std::vector<RE::Actor*>& a_actors)
		{
			std::vector<RE::Actor*> ordered(a_pick.order.size());
			for (size_t slot = 0; slot < a_pick.order.size(); slot++) {
				ordered[slot] = a_actors[a_pick.order[slot]];
			}
			return Scene::SceneRuntime::GetSingleton().StartFromDef(a_pick.id, ordered);
		}

		bool Play(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor, RE::BSFixedString a_file, RE::BSFixedString a_animId)
		{
			if (!a_actor) {
				REX::WARN("OSF.Play: no actor given");
				return false;
			}
			if (!Animation::GraphManager::GetSingleton().PlayAnimation(a_actor, a_file.c_str(), a_animId.c_str())) {
				REX::WARN("OSF.Play: could not find/play animation '{}' (id '{}')", a_file.c_str(), a_animId.c_str());
				return false;
			}
			return true;
		}

		bool Stop(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			if (!a_actor) {
				REX::WARN("OSF.Stop: no actor given");
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
				REX::WARN("OSF.SetSceneStageForActor: no actor given");
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
			REX::INFO("ReloadPacks: clip cache cleared");
			auto& registry = Registry::SceneRegistry::GetSingleton();
			registry.LoadAll();
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
				REX::WARN("OSF.SetSpeed: no actor given");
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
				REX::WARN("OSF.SetAnchor: no actor given");
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
				REX::WARN("OSF.StopSceneForActor: no actor given");
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
				REX::WARN("OSF.StartScene: no actors given");
				return 0;
			}
			const std::string sid = a_id.c_str();
			if (!Registry::SceneRegistry::GetSingleton().Find(sid)) {
				REX::WARN("OSF.StartScene: no scene '{}'", sid);
				return 0;
			}
			const auto anchor = MakeAnchor(ReadSceneOptions(a_opts));
			auto& rt = Scene::SceneRuntime::GetSingleton();
			if (anchor.set) {
				return rt.StartFromDefAt(sid, a_actors, anchor.pos, anchor.heading);  // anchored at the ref
			}
			return rt.StartFromDef(sid, a_actors);
		}

		// Start a scene binding actors to NAMED roles: asRoles[i] is the role for akActors[i]
		// (equal lengths). Returns the handle (0 = no such scene / validation fail). Its own native rather
		// than a SceneOptions field because Papyrus structs cannot carry the role array.
		int32_t StartSceneRoles(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			RE::BSFixedString a_id, std::vector<RE::BSFixedString> a_roles)
		{
			if (a_actors.empty()) {
				REX::WARN("OSF.StartSceneRoles: no actors given");
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
		int32_t StartMatched(const std::vector<RE::Actor*>& a_actors, const Matchmaking::TagQuery& a_query, const char* a_logTag)
		{
			if (a_actors.empty()) {
				REX::WARN("{}: no actors given", a_logTag);
				return 0;
			}
			for (auto* actor : a_actors) {
				if (!actor) {
					REX::WARN("{}: null actor in list", a_logTag);
					return 0;
				}
			}
			auto pick = Matchmaking::Pick(a_actors, a_query);
			if (!pick) {
				REX::WARN("{}: no matching animation found for the given tags/actors", a_logTag);
				return 0;
			}
			const int32_t handle = StartCandidate(*pick, a_actors);
			if (handle) {
				REX::INFO("{}: playing '{}' handle {:#010x}", a_logTag, pick->id, handle);
			} else {
				REX::WARN("{}: could not start matched scene '{}'", a_logTag, pick->id);
			}
			return handle;
		}

		// Anchored counterpart of StartCandidate: start the matchmade pick world-anchored at (a_pos,
		// a_heading) instead of co-locating the actors at actor[0], so it can sit on a bed/furniture/marker.
		int32_t StartCandidateAt(const Matchmaking::Candidate& a_pick, const std::vector<RE::Actor*>& a_actors, const RE::NiPoint3& a_pos, float a_heading)
		{
			std::vector<RE::Actor*> ordered(a_pick.order.size());
			for (size_t slot = 0; slot < a_pick.order.size(); slot++) {
				ordered[slot] = a_actors[a_pick.order[slot]];
			}
			return Scene::SceneRuntime::GetSingleton().StartFromDefAt(a_pick.id, ordered, a_pos, a_heading);
		}

		// Anchored counterpart of StartMatched: matchmake a_query, then start the pick anchored at (a_pos, a_heading).
		// Shared body of the StartSceneByTags*At natives.
		int32_t StartMatchedAt(const std::vector<RE::Actor*>& a_actors, const Matchmaking::TagQuery& a_query,
			const RE::NiPoint3& a_pos, float a_heading, const char* a_logTag)
		{
			if (a_actors.empty()) {
				REX::WARN("{}: no actors given", a_logTag);
				return 0;
			}
			for (auto* actor : a_actors) {
				if (!actor) {
					REX::WARN("{}: null actor in list", a_logTag);
					return 0;
				}
			}
			auto pick = Matchmaking::Pick(a_actors, a_query);
			if (!pick) {
				REX::WARN("{}: no matching animation found for the given tags/actors", a_logTag);
				return 0;
			}
			const int32_t handle = StartCandidateAt(*pick, a_actors, a_pos, a_heading);
			if (handle) {
				REX::INFO("{}: playing '{}' handle {:#010x} (anchored)", a_logTag, pick->id, handle);
			}
			return handle;
		}

		// Matchmake by tags + role/gender fit across BOTH registries (composed scene defs + packs), pick by priority tier + weighted-random, and start it with the matchmade binding.
		// Returns the scene handle (0 = no match / start failed); GetSceneId(handle) recovers the chosen id.
		// akOpts.Anchor world-anchors the matchmade scene at a ref (furniture/bed/marker) instead of co-locating at actor[0].
		int32_t StartSceneByTags(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors, std::vector<RE::BSFixedString> a_tags, SceneOptionsArg a_opts)
		{
			Matchmaking::TagQuery q;
			q.allOf = ToStrings(a_tags);
			const auto anchor = MakeAnchor(ReadSceneOptions(a_opts));
			if (anchor.set) {
				return StartMatchedAt(a_actors, q, anchor.pos, anchor.heading, "OSF.StartSceneByTags");
			}
			return StartMatched(a_actors, q, "OSF.StartSceneByTags");
		}

		// Boolean-query form of StartSceneByTags: all-of / any-of / none-of tag sets, otherwise identical (filter-aware matchmaking across both registries, priority + weighted pick).
		// akOpts.Anchor world-anchors the matchmade scene.
		int32_t StartSceneByTagsQuery(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_allOf, std::vector<RE::BSFixedString> a_anyOf,
			std::vector<RE::BSFixedString> a_noneOf, SceneOptionsArg a_opts)
		{
			Matchmaking::TagQuery q{ ToStrings(a_allOf), ToStrings(a_anyOf), ToStrings(a_noneOf) };
			const auto anchor = MakeAnchor(ReadSceneOptions(a_opts));
			if (anchor.set) {
				return StartMatchedAt(a_actors, q, anchor.pos, anchor.heading, "OSF.StartSceneByTagsQuery");
			}
			return StartMatched(a_actors, q, "OSF.StartSceneByTagsQuery");
		}

		// --- Scene-event callbacks (OSFTypes:SceneEvent payload) --------------------
		int32_t RegisterSceneCallback(OSFVM&, uint32_t, std::monostate, RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_fn, int32_t a_scene, int32_t a_eventMask)
		{
			if (!a_receiver.get()) {
				REX::WARN("OSF.RegisterSceneCallback: null receiver");
				return 0;
			}
			return Scene::SceneEventRelay::GetSingleton().Register(a_receiver, a_fn.c_str(), a_scene, a_eventMask);
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
	}

	void RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm)
	{
		// Force-load the OSFTypes script type so its SceneOptions + SceneEvent structs are registered
		// BEFORE we bind the struct-typed Start* natives AND before the relay's CreateStruct("OSFTypes#SceneEvent") at dispatch time.
		{
			RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> typesType;
			if (!a_vm->GetScriptObjectType(RE::BSFixedString(TYPES_SCRIPT_NAME.data()), typesType) || !typesType) {
				REX::WARN("RegisterFunctions: could not preload '{}' type info — struct-typed natives "
						  "(StartScene/StartSceneByTags/StartSceneByTagsQuery) may fail to register and "
						  "scene-event callbacks may receive no payload", TYPES_SCRIPT_NAME);
			}
		}

		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneByTags", &StartSceneByTags, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartScene", &StartScene, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneByTagsQuery", &StartSceneByTagsQuery, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneRoles", &StartSceneRoles, true, false);

		a_vm->BindNativeMethod(SCRIPT_NAME, "RegisterSceneCallback", &RegisterSceneCallback, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "UnregisterSceneCallback", &UnregisterSceneCallback, true, false);

		a_vm->BindNativeMethod(SCRIPT_NAME, "AdvanceScene", &AdvanceScene, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "NavigateScene", &NavigateScene, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneEdgeCount", &GetSceneEdgeCount, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneEdgeId", &GetSceneEdgeId, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneEdgeLabel", &GetSceneEdgeLabel, true, false);

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
		REX::INFO("Registered papyrus natives on script '{}'", SCRIPT_NAME);
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
