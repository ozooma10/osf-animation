#include "OSFScript.h"

#include "Animation/GraphManager.h"
#include "Audio/SoundService.h"
#include "Matchmaking/Matchmaker.h"
#include "Registry/PackRegistry.h"
#include "Registry/SceneRegistry.h"
#include "Scene/SceneEventRelay.h"
#include "Scene/SceneRuntime.h"
#include "Serialization/GLTFImport.h"
#include "Util/Math.h"
#include "Util/StringUtil.h"

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

		// Split a "scene:"/"anim:" registry prefix off a start id. "scene:" forces the scene registry, "anim:" forces the pack registry; 
		// a bare id leaves both false (the caller's scene-then-pack resolution). Shared by StartScene / StartSceneAt.
		struct ScenePrefix
		{
			std::string id;
			bool        forceScene = false;
			bool        forcePack = false;
		};

		ScenePrefix SplitScenePrefix(std::string_view a_raw)
		{
			ScenePrefix p;
			p.id = a_raw;
			if (p.id.rfind("scene:", 0) == 0) {
				p.forceScene = true;
				p.id = p.id.substr(6);
			} else if (p.id.rfind("anim:", 0) == 0) {
				p.forcePack = true;
				p.id = p.id.substr(5);
			}
			return p;
		}

		// Start a matchmade candidate using its resolved binding (Matchmaking::Pick already chose the slot->actor order, so we never re-bind here). 
		// Scene defs go through StartFromDef (binds by declaration order = the reordered actors); packs through StartFromPack.
		int32_t StartCandidate(const Matchmaking::Candidate& a_pick, const std::vector<RE::Actor*>& a_actors)
		{
			std::vector<RE::Actor*> ordered(a_pick.order.size());
			for (size_t slot = 0; slot < a_pick.order.size(); slot++) {
				ordered[slot] = a_actors[a_pick.order[slot]];
			}
			auto& rt = Scene::SceneRuntime::GetSingleton();
			if (a_pick.source == Matchmaking::Candidate::Source::kSceneDef) {
				return rt.StartFromDef(a_pick.id, ordered);
			}
			return rt.StartFromPack(a_pick.id, ordered, 0);
		}

		bool Play(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor, RE::BSFixedString a_file, RE::BSFixedString a_animId)
		{
			if (!a_actor) {
				REX::WARN("OSF.Play: no actor given");
				return false;
			}
			return Animation::GraphManager::GetSingleton().PlayAnimation(a_actor, a_file.c_str(), a_animId.c_str());
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

		// Rescans Data/OSF/**/*.json (dev convenience: edit a pack, reload, replay without restarting the game).
		// Returns the number of animations loaded. Also drops the GLB clip cache so edited animation files re-import.
		int32_t ReloadPacks(OSFVM&, uint32_t, std::monostate)
		{
			Serialization::GLTFImport::ClearCache();
			REX::INFO("ReloadPacks: clip cache cleared");
			auto& registry = Registry::PackRegistry::GetSingleton();
			registry.LoadAll();
			Registry::SceneRegistry::GetSingleton().LoadAll();  // ReloadPacks reloads scenes too
			return static_cast<int32_t>(registry.Size());
		}

		// Discovery: ids of scenes (composed defs + packs) with aiActorCount actors whose tags contain ALL asTags. 
		// Deterministic (priority desc, then id asc). Count + tags only — filter-UNAWARE (no Actor[]); 
		// use FindScenesForActorsQuery / StartSceneByTags* for a filter-correct result.
		std::vector<RE::BSFixedString> FindScenes(OSFVM&, uint32_t, std::monostate, int32_t a_actorCount, std::vector<RE::BSFixedString> a_tags)
		{
			std::vector<RE::BSFixedString> result;
			if (a_actorCount <= 0) {
				return result;
			}
			Matchmaking::TagQuery q;
			q.allOf = ToStrings(a_tags);
			const std::vector<RE::Actor*> noActors;
			for (auto& id : Matchmaking::FindIds(a_actorCount, q, noActors)) {
				result.emplace_back(id);
			}
			return result;
		}

		// Discovery, filter-aware: like FindScenes but takes the actors, so keyword/race/gender role
		// filters AND a complete binding are required for a scene def to appear. Boolean tag sets.
		std::vector<RE::BSFixedString> FindScenesForActorsQuery(OSFVM&, uint32_t, std::monostate,
			std::vector<RE::Actor*> a_actors, std::vector<RE::BSFixedString> a_allOf,
			std::vector<RE::BSFixedString> a_anyOf, std::vector<RE::BSFixedString> a_noneOf)
		{
			std::vector<RE::BSFixedString> result;
			if (a_actors.empty()) {
				return result;
			}
			Matchmaking::TagQuery q{ ToStrings(a_allOf), ToStrings(a_anyOf), ToStrings(a_noneOf) };
			for (auto& id : Matchmaking::FindIds(static_cast<int32_t>(a_actors.size()), q, a_actors)) {
				result.emplace_back(id);
			}
			return result;
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

		// Bring N already-playing graphs together. Call OSF.Play on each actor first, then OSF.Sync.
		// abAnchor=true (default): anchor the graphs into one shared scene at actor[0]'s spot
		// Scene participants are skipped; needs >= 2 graphs.
		bool Sync(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors, bool a_anchor)
		{
			return Animation::GraphManager::GetSingleton().Sync(a_actors, a_anchor);
		}

		// Solo multi-phase sequence (primitive). Parallel arrays, equal length:
		// asFiles[i] phase clip, aiLoops[i] loops before advancing, afBlends[i] blend-in secs. abLoopWhole restarts at phase 0 after the last.
		bool PlaySequence(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor,
			std::vector<RE::BSFixedString> a_files, std::vector<int32_t> a_loops, std::vector<float> a_blends, bool a_loopWhole)
		{
			if (!a_actor) {
				REX::WARN("OSF.PlaySequence: no actor given");
				return false;
			}
			std::vector<std::string> files;
			files.reserve(a_files.size());
			for (const auto& f : a_files) {
				files.emplace_back(f.c_str());
			}
			return Animation::GraphManager::GetSingleton().PlaySequence(a_actor, files, a_loops, a_blends, a_loopWhole);
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

		// Engine crosshair target: the reference under the reticle / activate prompt.
		// Reads PlayerCharacter->commandTarget. Any ref kind (actor/door/container/...), or null when the crosshair is on nothing.
		RE::TESObjectREFR* CrosshairTarget()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			return player ? player->commandTarget : nullptr;
		}

		// Non-public helper: the raw engine crosshair reference, or None. Used by the OSFTest harness to pick a target from the reticle.
		RE::TESObjectREFR* GetCrosshairRef(OSFVM&, uint32_t, std::monostate)
		{
			return CrosshairTarget();
		}

		// Non-public helper: the crosshair reference cast to Actor, or None when the crosshair is on nothing or a non-actor ref (kACHR form-type gate).
		// Used by the OSFTest harness to target the actor under the reticle.
		RE::Actor* GetCrosshairActor(OSFVM&, uint32_t, std::monostate)
		{
			auto* target = CrosshairTarget();
			return (target && target->IsActor()) ? static_cast<RE::Actor*>(target) : nullptr;
		}

		// Framework semver "major.minor.patch" (string so it can't be misread).
		RE::BSFixedString GetVersion(OSFVM&, uint32_t, std::monostate)
		{
			const auto v = SFSE::GetPluginVersion();
			return RE::BSFixedString(std::format("{}.{}.{}", v.major(), v.minor(), v.patch()));
		}

		// Save-safety fallback: drop scene/graph state anchored in the discarded world. 
		// WARNING: only on an actual load — against a LIVE scene it leaves participants animation-driven (StopAll skips actor restore). 
		// Use StopScene for normal teardown.
		void NotifyGameLoaded(OSFVM&, uint32_t, std::monostate)
		{
			Animation::GraphManager::GetSingleton().StopAll("game loaded");
		}

		// True once OSF is loaded and initialized (playback hooks installed).
		bool IsReady(OSFVM&, uint32_t, std::monostate)
		{
			return Animation::GraphManager::GetSingleton().HooksInstalled();
		}

		// Start a scene by id, returning an opaque scene HANDLE (0 = failed). 
		// Routes through SceneRuntime so callbacks fire and the handle drives GetSceneId/Node/StopScene/etc.
		// ID resolution: a `scene:` prefix forces the scene registry, `anim:` forces the pack registry; 
		// a bare id resolves the scene registry first, then the pack registry (a linear pack auto-exposed as a single-path scene).
		// aiStage = pack start stage (ignored for def-backed graphs).
		int32_t StartScene(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors, RE::BSFixedString a_id,
			int32_t a_stage)
		{
			if (a_actors.empty()) {
				REX::WARN("OSF.StartScene: no actors given");
				return 0;
			}
			const auto [sid, forceScene, forcePack] = SplitScenePrefix(a_id.c_str());

			auto& rt = Scene::SceneRuntime::GetSingleton();
			if (!forcePack && Registry::SceneRegistry::GetSingleton().Find(sid)) {
				return rt.StartFromDef(sid, a_actors);  // composed graph
			}
			if (!forceScene) {
				return rt.StartFromPack(sid, a_actors, a_stage);  // linear pack as single-path scene
			}
			REX::WARN("OSF.StartScene: no scene '{}' (scene: prefix forced)", sid);
			return 0;
		}

		// Like StartScene, but world-anchors the scene at an ObjectReference (furniture / bed / marker) instead of co-locating the actors at actor[0]. 
		// afHeadingDeg < 0 uses akAnchor's own heading; otherwise it is a heading in DEGREES. Id resolution (scene-then-pack, the scene:/anim: prefixes) mirrors StartScene. 
		// Returns the handle (0 = failed).
		int32_t StartSceneAt(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors, RE::BSFixedString a_id,
			RE::TESObjectREFR* a_anchor, float a_headingDeg)
		{
			if (a_actors.empty()) {
				REX::WARN("OSF.StartSceneAt: no actors given");
				return 0;
			}
			if (!a_anchor) {
				REX::WARN("OSF.StartSceneAt: no anchor reference given");
				return 0;
			}
			const auto [sid, forceScene, forcePack] = SplitScenePrefix(a_id.c_str());

			// World anchor = the reference's position + (its heading, or an explicit one in degrees).
			const RE::NiPoint3 pos = a_anchor->data.location;
			const float heading = (a_headingDeg < 0.0f) ? a_anchor->data.angle.z : (a_headingDeg * Util::kDegToRadF);

			auto& rt = Scene::SceneRuntime::GetSingleton();
			if (!forcePack && Registry::SceneRegistry::GetSingleton().Find(sid)) {
				return rt.StartFromDefAt(sid, a_actors, pos, heading);  // composed graph, anchored at the ref
			}
			if (!forceScene) {
				return rt.StartFromPack(sid, a_actors, 0, Scene::SceneRuntime::AnchorOverride{ true, pos, heading });
			}
			REX::WARN("OSF.StartSceneAt: no scene '{}' (scene: prefix forced)", sid);
			return 0;
		}

		// Start a def-backed scene binding actors to NAMED roles: asRoles[i] is the role for akActors[i] (equal lengths). 
		// Returns the handle (0 = no such scene / validation fail).
		// Roles are a *.scene.json concept; a `scene:` prefix is tolerated/stripped.
		int32_t StartSceneRoles(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			RE::BSFixedString a_id, std::vector<RE::BSFixedString> a_roles)
		{
			if (a_actors.empty()) {
				REX::WARN("OSF.StartSceneRoles: no actors given");
				return 0;
			}
			std::string sid = a_id.c_str();
			if (sid.rfind("scene:", 0) == 0) {
				sid = sid.substr(6);
			}
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
				return 0;
			}
			const int32_t handle = StartCandidate(*pick, a_actors);
			if (handle) {
				REX::INFO("{}: playing '{}' ({}) handle {:#010x}", a_logTag,
					pick->id, pick->source == Matchmaking::Candidate::Source::kSceneDef ? "scene" : "pack", handle);
			}
			return handle;
		}

		// Anchored counterpart of StartCandidate: start the matchmade pick world-anchored at (a_pos, a_heading) instead of co-locating the actors at actor[0]. 
		// Uses the SAME two anchored entry points StartSceneAt does (StartFromDefAt for scene defs, StartFromPack + AnchorOverride for packs),
		// so a matchmade scene can sit on a bed / furniture / marker.
		int32_t StartCandidateAt(const Matchmaking::Candidate& a_pick, const std::vector<RE::Actor*>& a_actors, const RE::NiPoint3& a_pos, float a_heading)
		{
			std::vector<RE::Actor*> ordered(a_pick.order.size());
			for (size_t slot = 0; slot < a_pick.order.size(); slot++) {
				ordered[slot] = a_actors[a_pick.order[slot]];
			}
			auto& rt = Scene::SceneRuntime::GetSingleton();
			if (a_pick.source == Matchmaking::Candidate::Source::kSceneDef) {
				return rt.StartFromDefAt(a_pick.id, ordered, a_pos, a_heading);
			}
			return rt.StartFromPack(a_pick.id, ordered, 0, Scene::SceneRuntime::AnchorOverride{ true, a_pos, a_heading });
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
				return 0;
			}
			const int32_t handle = StartCandidateAt(*pick, a_actors, a_pos, a_heading);
			if (handle) {
				REX::INFO("{}: playing '{}' ({}) handle {:#010x} (anchored)", a_logTag, pick->id, pick->source == Matchmaking::Candidate::Source::kSceneDef ? "scene" : "pack", handle);
			}
			return handle;
		}

		// Matchmake by tags + role/gender fit across BOTH registries (composed scene defs + packs), pick by priority tier + weighted-random, and start it with the matchmade binding.
		// Returns the scene handle (0 = no match / start failed); GetSceneId(handle) recovers the chosen id.
		int32_t StartSceneByTags(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_tags)
		{
			Matchmaking::TagQuery q;
			q.allOf = ToStrings(a_tags);
			return StartMatched(a_actors, q, "OSF.StartSceneByTags");
		}

		// Boolean-query form of StartSceneByTags: all-of / any-of / none-of tag sets, otherwise identical (filter-aware matchmaking across both registries, priority + weighted pick).
		int32_t StartSceneByTagsQuery(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_allOf, std::vector<RE::BSFixedString> a_anyOf,
			std::vector<RE::BSFixedString> a_noneOf)
		{
			Matchmaking::TagQuery q{ ToStrings(a_allOf), ToStrings(a_anyOf), ToStrings(a_noneOf) };
			return StartMatched(a_actors, q, "OSF.StartSceneByTagsQuery");
		}

		// Like StartSceneByTags, but world-anchors the matchmade scene at akAnchor (furniture / bed / marker) instead of co-locating the actors at actor[0]. 
		// afHeadingDeg < 0 uses the anchor's own heading; otherwise it is a heading in DEGREES. 
		// For furniture/sleep encounters that belong to a thing, not an actor.
		int32_t StartSceneByTagsAt(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_tags, RE::TESObjectREFR* a_anchor, float a_headingDeg)
		{
			if (a_actors.empty()) {
				REX::WARN("OSF.StartSceneByTagsAt: no actors given");
				return 0;
			}
			if (!a_anchor) {
				REX::WARN("OSF.StartSceneByTagsAt: no anchor reference given");
				return 0;
			}
			const RE::NiPoint3 pos = a_anchor->data.location;
			const float heading = (a_headingDeg < 0.0f) ? a_anchor->data.angle.z : (a_headingDeg * Util::kDegToRadF);
			Matchmaking::TagQuery q;
			q.allOf = ToStrings(a_tags);
			return StartMatchedAt(a_actors, q, pos, heading, "OSF.StartSceneByTagsAt");
		}

		// Boolean-query form of StartSceneByTagsAt (all-of / any-of / none-of), world-anchored at akAnchor.
		int32_t StartSceneByTagsQueryAt(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_allOf, std::vector<RE::BSFixedString> a_anyOf,
			std::vector<RE::BSFixedString> a_noneOf, RE::TESObjectREFR* a_anchor, float a_headingDeg)
		{
			if (a_actors.empty()) {
				REX::WARN("OSF.StartSceneByTagsQueryAt: no actors given");
				return 0;
			}
			if (!a_anchor) {
				REX::WARN("OSF.StartSceneByTagsQueryAt: no anchor reference given");
				return 0;
			}
			const RE::NiPoint3 pos = a_anchor->data.location;
			const float heading = (a_headingDeg < 0.0f) ? a_anchor->data.angle.z : (a_headingDeg * Util::kDegToRadF);
			Matchmaking::TagQuery q{ ToStrings(a_allOf), ToStrings(a_anyOf), ToStrings(a_noneOf) };
			return StartMatchedAt(a_actors, q, pos, heading, "OSF.StartSceneByTagsQueryAt");
		}

		// Ad-hoc one-shot scene from raw animation files: co-locates the actors at actor[0],
		// plays each file at afSpeed with afBlendIn, and syncs the clock. Returns the scene handle (0 = failed).
		int32_t StartSceneFiles(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_files, float a_speed, float a_blendIn)
		{
			if (a_actors.size() != a_files.size()) {
				REX::WARN("OSF.StartSceneFiles: actor/file array sizes differ ({} vs {})", a_actors.size(), a_files.size());
				return 0;
			}
			std::vector<std::string> files;
			files.reserve(a_files.size());
			for (const auto& f : a_files) {
				files.emplace_back(f.c_str());
			}
			return Scene::SceneRuntime::GetSingleton().StartFromFiles(a_actors, files, a_speed, a_blendIn);
		}

		// --- Scene-event callbacks (OSFEvent:SceneEvent payload) ------------------
		// Register akReceiver.asFn(OSFEvent:SceneEvent) for events in aiEventMask
		// (& scene aiScene, 0 = any). Returns a generational token (0 = failed).
		int32_t RegisterSceneCallback(OSFVM&, uint32_t, std::monostate, RE::BSTSmartPointer<RE::BSScript::Object> a_receiver,
			RE::BSFixedString a_fn, int32_t a_scene, int32_t a_eventMask)
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

		// DEBUG: lets a Papyrus receiver echo into OSF Animation.log (REX), so the
		// transport round-trip is provable without enabling the Papyrus script log.
		void Dbg_Log(OSFVM&, uint32_t, std::monostate, RE::BSFixedString a_msg)
		{
			REX::INFO("[Papyrus] {}", a_msg.c_str());
		}

		// DEBUG (OSFCompat): play a Data-relative loose file through SoundService at the player's
		// position — the in-world audible test for the Wwise external-source path. Routes through the
		// SAME code as scene sound cues ("event:" specs, codec-by-extension, external-source post on
		// the shipped event), so hearing a beep here proves audible + engine-mixed playback end to end
		// (it should duck/pause with the game and follow the volume sliders). e.g. from the console:
		//   cgf "OSFCompat.Dbg_PlaySound" "OSF\Sounds\testbeep.wav"
		void Dbg_PlaySound(OSFVM&, uint32_t, std::monostate, RE::BSFixedString a_dataRelPath)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			const RE::NiPoint3 pos = player ? player->data.location : RE::NiPoint3{};
			REX::INFO("OSFCompat.Dbg_PlaySound: '{}' at player ({:.1f},{:.1f},{:.1f})",
				a_dataRelPath.c_str(), pos.x, pos.y, pos.z);
			Audio::SoundService::GetSingleton().Play(a_dataRelPath.c_str(), pos, 1.0f);
		}

		// DEBUG (OSFCompat): no-instance transport probe — DispatchStaticCall
		// asScript.asFn(OSFEvent:SceneEvent) directly (no registration). Proves the struct
		// marshalling from the console without a scripted form.
		void Dbg_FireSceneEventStatic(OSFVM&, uint32_t, std::monostate, RE::BSFixedString a_script,
			RE::BSFixedString a_fn, int32_t a_scene, int32_t a_event, RE::BSFixedString a_node)
		{
			REX::INFO("OSFCompat.Dbg_FireSceneEventStatic: -> {}.{}(SceneEvent) scene={} event={:#x} node='{}'",
				a_script.c_str(), a_fn.c_str(), a_scene, a_event, a_node.c_str());
			Scene::SceneEvent e;
			e.scene = a_scene;
			e.event = a_event;
			e.node = a_node.c_str();
			e.anchor = (a_event == Scene::Event::kNodeEnter) ? "enter" : (a_event == Scene::Event::kNodeExit) ? "exit" : "";
			Scene::SceneEventRelay::GetSingleton().DispatchStatic(a_script.c_str(), a_fn.c_str(), e);
		}

		// DEBUG (OSFCompat): fire a synthetic EVENT_ACTION carrying a REAL actor through the
		// static dispatch, to prove the actorRef object marshalling (Actor* -> Var -> struct
		// member) without needing a scripted-form instance for the real RegisterSceneCallback path.
		void Dbg_FireActionActor(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor, RE::BSFixedString a_script,
			RE::BSFixedString a_fn, RE::BSFixedString a_role)
		{
			REX::INFO("OSFCompat.Dbg_FireActionActor: -> {}.{}(SceneEvent) actor={:X} role='{}'",
				a_script.c_str(), a_fn.c_str(), a_actor ? a_actor->formID : 0, a_role.c_str());
			Scene::SceneEvent e;
			e.scene = 0x9999;
			e.event = Scene::Event::kAction;
			e.node = "main";
			e.actionType = "test.ping";
			e.role = a_role.c_str();
			e.actor = a_actor;  // packed into actorRef by PackPayload
			Scene::SceneEventRelay::GetSingleton().DispatchStatic(a_script.c_str(), a_fn.c_str(), e);
		}

		// --- Scene state getters (handle-based, against the scene runtime's instance table) --
		// Scene instance id, or "" if the handle is invalid/ended.
		RE::BSFixedString GetSceneId(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			return RE::BSFixedString(Scene::SceneRuntime::GetSingleton().GetId(a_scene).c_str());
		}

		// Current node id of the scene, or "" if invalid.
		RE::BSFixedString GetSceneNode(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			return RE::BSFixedString(Scene::SceneRuntime::GetSingleton().GetNode(a_scene).c_str());
		}

		// The scene handle a_actor participates in, or 0 if none.
		int32_t GetSceneForActor(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			return Scene::SceneRuntime::GetSingleton().GetSceneForActor(a_actor);
		}

		// --- Scene-metadata introspection (read-only; reads *.scene.json defs by id) ----------
		// Let an orchestrator inspect a scene's role/gender/tag conventions before binding actors.
		// All resolve a *.scene.json def by id; an unknown id (or a pack id, which is not a scene
		// def) yields the empty/sentinel result. Returned arrays are real (possibly empty) — safe
		// per the None-array footgun, which is about INBOUND None arrays.

		// Declared role names of a scene (in declaration order). Empty if unknown.
		std::vector<RE::BSFixedString> GetSceneRoles(OSFVM&, uint32_t, std::monostate, RE::BSFixedString a_id)
		{
			std::vector<RE::BSFixedString> out;
			if (const auto* def = Registry::SceneRegistry::GetSingleton().Find(a_id.c_str())) {
				for (const auto& r : def->roles) {
					out.emplace_back(r.name);
				}
			}
			return out;
		}

		// Gender slot of a named role: "male" / "female" / "any". "" for an unknown scene or role.
		RE::BSFixedString GetSceneRoleGender(OSFVM&, uint32_t, std::monostate, RE::BSFixedString a_id, RE::BSFixedString a_role)
		{
			const auto* def = Registry::SceneRegistry::GetSingleton().Find(a_id.c_str());
			if (!def) {
				return "";
			}
			const auto want = Util::ToLower(a_role.c_str());
			for (const auto& r : def->roles) {
				if (Util::ToLower(r.name) == want) {
					switch (r.gender) {
					case Registry::SlotGender::kMale:
						return "male";
					case Registry::SlotGender::kFemale:
						return "female";
					default:
						return "any";
					}
				}
			}
			return "";
		}

		// Declared role/actor count of a scene; 0 for an unknown id or a scene with no roles.
		int32_t GetSceneActorCount(OSFVM&, uint32_t, std::monostate, RE::BSFixedString a_id)
		{
			const auto* def = Registry::SceneRegistry::GetSingleton().Find(a_id.c_str());
			return def ? static_cast<int32_t>(def->roles.size()) : 0;
		}

		// Tags of a scene. Empty if unknown.
		std::vector<RE::BSFixedString> GetSceneTags(OSFVM&, uint32_t, std::monostate, RE::BSFixedString a_id)
		{
			std::vector<RE::BSFixedString> out;
			if (const auto* def = Registry::SceneRegistry::GetSingleton().Find(a_id.c_str())) {
				for (const auto& t : def->tags) {
					out.emplace_back(t);
				}
			}
			return out;
		}

		// DEBUG (OSFCompat): drive the scene-runtime lifecycle directly, without going through
		// the GraphManager/StartScene handle minting. Each fires the matching lifecycle
		// event(s) through the relay.
		int32_t Dbg_StartScene(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor, RE::BSFixedString a_id, RE::BSFixedString a_node)
		{
			std::vector<RE::Actor*> participants;
			if (a_actor) {
				participants.push_back(a_actor);
			}
			return Scene::SceneRuntime::GetSingleton().Start(a_id.c_str(), a_node.c_str(), participants);
		}

		bool Dbg_SetSceneNode(OSFVM&, uint32_t, std::monostate, int32_t a_scene, RE::BSFixedString a_node, int32_t a_stage)
		{
			return Scene::SceneRuntime::GetSingleton().SetNode(a_scene, a_node.c_str(), a_stage);
		}

		bool Dbg_StopScene(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			return Scene::SceneRuntime::GetSingleton().Stop(a_scene);
		}

		// Start a scene from its *.scene.json def (entering at the def's entry node). 
		// DEBUG helper on OSFCompat. Returns the handle (0 = fail).
		int32_t Dbg_StartSceneDef(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor, RE::BSFixedString a_sceneId)
		{
			std::vector<RE::Actor*> participants;
			if (a_actor) {
				participants.push_back(a_actor);
			}
			return Scene::SceneRuntime::GetSingleton().StartFromDef(a_sceneId.c_str(), participants);
		}

		// --- Scene navigation (handle-based; def-backed scenes) --------------------
		bool AdvanceScene(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			return Scene::SceneRuntime::GetSingleton().Advance(a_scene);
		}

		bool NavigateScene(OSFVM&, uint32_t, std::monostate, int32_t a_scene, RE::BSFixedString a_edgeId)
		{
			return Scene::SceneRuntime::GetSingleton().Navigate(a_scene, a_edgeId.c_str());
		}

		int32_t GetSceneEdgeCount(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			return Scene::SceneRuntime::GetSingleton().EdgeCount(a_scene);
		}

		RE::BSFixedString GetSceneEdgeId(OSFVM&, uint32_t, std::monostate, int32_t a_scene, int32_t a_index)
		{
			return RE::BSFixedString(Scene::SceneRuntime::GetSingleton().EdgeId(a_scene, a_index).c_str());
		}

		RE::BSFixedString GetSceneEdgeLabel(OSFVM&, uint32_t, std::monostate, int32_t a_scene, int32_t a_index)
		{
			return RE::BSFixedString(Scene::SceneRuntime::GetSingleton().EdgeLabel(a_scene, a_index).c_str());
		}

		// --- Scene-load diagnostics ------------------------------------------------
		// Problems (errors + warnings, each prefixed) from the last load / ReloadPacks.
		std::vector<RE::BSFixedString> GetSceneLoadErrors(OSFVM&, uint32_t, std::monostate)
		{
			std::vector<RE::BSFixedString> out;
			for (const auto& e : Registry::SceneRegistry::GetSingleton().LoadErrors()) {
				out.emplace_back(e.c_str());
			}
			return out;
		}

		// True iff a_id names a scene that loaded. Anything in the registry passed validation
		// (invalid scenes are skipped at load), so "loaded" means "valid". A scene that failed
		// to parse is absent -> false; use GetSceneValidationErrors / GetSceneLoadErrors to see
		// why. (Pack-registry ids aren't scenes and return false here.)
		bool ValidateScene(OSFVM&, uint32_t, std::monostate, RE::BSFixedString a_id)
		{
			return Registry::SceneRegistry::GetSingleton().Find(a_id.c_str()) != nullptr;
		}

		// The load problems (errors + warnings) referring to a_id — the subset of GetSceneLoadErrors
		// whose text mentions the id (the reject/warn messages embed the scene id). Empty = the id
		// had no recorded problems (it loaded clean, or the id never appeared). For a file that
		// failed before its id could be read, use GetSceneLoadErrors (the full list).
		std::vector<RE::BSFixedString> GetSceneValidationErrors(OSFVM&, uint32_t, std::monostate, RE::BSFixedString a_id)
		{
			const std::string want = Util::ToLower(a_id.c_str());
			std::vector<RE::BSFixedString> out;
			for (const auto& e : Registry::SceneRegistry::GetSingleton().LoadErrors()) {
				if (Util::ToLower(e).find(want) != std::string::npos) {
					out.emplace_back(e.c_str());
				}
			}
			return out;
		}

		// DEBUG (OSFCompat): log a parsed scene's graph structure to the OSF Animation.log.
		void Dbg_DumpScene(OSFVM&, uint32_t, std::monostate, RE::BSFixedString a_id)
		{
			const auto* def = Registry::SceneRegistry::GetSingleton().Find(a_id.c_str());
			if (!def) {
				REX::INFO("Dbg_DumpScene: no scene '{}'", a_id.c_str());
				return;
			}
			REX::INFO("Dbg_DumpScene '{}' name='{}' entry='{}' tags={} roles={} nodes={}",
				def->id, def->name, def->entry, def->tags.size(), def->roles.size(), def->nodes.size());
			for (const auto& n : def->nodes) {
				const char* mode = n.loopMode == Registry::LoopMode::kOnce ? "once" :
					n.loopMode == Registry::LoopMode::kHold ? "hold" : "count";
				REX::INFO("  node '{}' anim='{}' loop={}({}) timer={} edges={}",
					n.id, n.anim, mode, n.loopCount, n.timerSec, n.edges.size());
				for (const auto& e : n.edges) {
					const char* when = e.when == Registry::EdgeWhen::kEnd ? "end" :
						e.when == Registry::EdgeWhen::kLoops ? "loops" :
						e.when == Registry::EdgeWhen::kTimer ? "timer" :
						e.when == Registry::EdgeWhen::kAdvance ? "advance" : "trigger";
					REX::INFO("    edge -> '{}' when={} id='{}' default={}", e.to, when, e.id, e.isDefault);
				}
			}
		}
	}

	void RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm)
	{
		a_vm->BindNativeMethod(SCRIPT_NAME, "Play", &Play, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "Stop", &Stop, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "SetSceneStage", &SetSceneStage, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "SetSceneStageForActor", &SetSceneStageForActor, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "ReloadPacks", &ReloadPacks, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneStage", &GetSceneStage, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneStageForActor", &GetSceneStageForActor, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "IsPlaying", &IsPlaying, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetCurrentAnimation", &GetCurrentAnimation, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "SetSpeed", &SetSpeed, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSpeed", &GetSpeed, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "SetAnchor", &SetAnchor, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "ClearAnchor", &ClearAnchor, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "Sync", &Sync, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "PlaySequence", &PlaySequence, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StopScene", &StopScene, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StopSceneForActor", &StopSceneForActor, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetVersion", &GetVersion, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "NotifyGameLoaded", &NotifyGameLoaded, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartScene", &StartScene, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneAt", &StartSceneAt, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneRoles", &StartSceneRoles, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneByTags", &StartSceneByTags, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneByTagsQuery", &StartSceneByTagsQuery, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneByTagsAt", &StartSceneByTagsAt, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneByTagsQueryAt", &StartSceneByTagsQueryAt, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneFiles", &StartSceneFiles, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "FindScenes", &FindScenes, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "FindScenesForActorsQuery", &FindScenesForActorsQuery, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "IsReady", &IsReady, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "RegisterSceneCallback", &RegisterSceneCallback, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "UnregisterSceneCallback", &UnregisterSceneCallback, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneId", &GetSceneId, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneNode", &GetSceneNode, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneForActor", &GetSceneForActor, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneRoles", &GetSceneRoles, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneRoleGender", &GetSceneRoleGender, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneActorCount", &GetSceneActorCount, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneTags", &GetSceneTags, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneLoadErrors", &GetSceneLoadErrors, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "ValidateScene", &ValidateScene, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneValidationErrors", &GetSceneValidationErrors, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "AdvanceScene", &AdvanceScene, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "NavigateScene", &NavigateScene, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneEdgeCount", &GetSceneEdgeCount, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneEdgeId", &GetSceneEdgeId, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneEdgeLabel", &GetSceneEdgeLabel, true, false);
		REX::INFO("Registered papyrus natives on script '{}'", SCRIPT_NAME);

		// Non-public crosshair + debug natives — kept off the public OSF surface (see
		// COMPAT_SCRIPT_NAME / OSFCompat.psc). The OSFTest harness calls these.
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "GetCrosshairRef", &GetCrosshairRef, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "GetCrosshairActor", &GetCrosshairActor, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "Dbg_FireSceneEventStatic", &Dbg_FireSceneEventStatic, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "Dbg_FireActionActor", &Dbg_FireActionActor, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "Dbg_StartScene", &Dbg_StartScene, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "Dbg_SetSceneNode", &Dbg_SetSceneNode, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "Dbg_StopScene", &Dbg_StopScene, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "Dbg_DumpScene", &Dbg_DumpScene, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "Dbg_StartSceneDef", &Dbg_StartSceneDef, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "Dbg_Log", &Dbg_Log, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "Dbg_PlaySound", &Dbg_PlaySound, true, false);
		REX::INFO("Registered non-public natives on script '{}'", COMPAT_SCRIPT_NAME);
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
