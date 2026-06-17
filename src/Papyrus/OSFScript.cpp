#include "OSFScript.h"

#include "Animation/GraphManager.h"
#include "Camera/CameraService.h"
#include "Player/PlayerControlService.h"
#include "Registry/PackRegistry.h"
#include "Registry/SceneRegistry.h"
#include "Scene/SceneEventRelay.h"
#include "Scene/SceneRuntime.h"
#include "Serialization/GLTFImport.h"
#include "Util/StringUtil.h"

#include <format>

namespace OSF::Papyrus
{
	namespace
	{
		using OSFVM = RE::BSScript::IVirtualMachine;

		std::vector<RE::SEX> ActorGenders(const std::vector<RE::Actor*>& a_actors)
		{
			std::vector<RE::SEX> genders;
			genders.reserve(a_actors.size());
			for (auto* actor : a_actors) {
				auto* npc = actor ? actor->GetNPC() : nullptr;
				genders.push_back(npc ? npc->GetSex() : RE::SEX::kNone);
			}
			return genders;
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

		// Jump a linear scene (by handle) to a given stage. False on a non-linear graph,
		// an out-of-range stage, or an invalid handle.
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

		// Rescans Data/OSF/**/*.json (dev convenience: edit a pack, reload, replay
		// without restarting the game). Returns the number of animations loaded.
		// Also drops the GLB clip cache so edited animation files re-import.
		int32_t ReloadPacks(OSFVM&, uint32_t, std::monostate)
		{
			Serialization::GLTFImport::ClearCache();
			REX::INFO("ReloadPacks: clip cache cleared");
			auto& registry = Registry::PackRegistry::GetSingleton();
			registry.LoadAll();
			Registry::SceneRegistry::GetSingleton().LoadAll();  // ReloadPacks reloads scenes too
			return static_cast<int32_t>(registry.Size());
		}

		std::vector<RE::BSFixedString> FindScenes(OSFVM&, uint32_t, std::monostate, int32_t a_actorCount, std::vector<RE::BSFixedString> a_tags)
		{
			std::vector<std::string> tags;
			tags.reserve(a_tags.size());
			for (const auto& tag : a_tags) {
				tags.emplace_back(tag.c_str());
			}
			std::vector<RE::BSFixedString> result;
			if (a_actorCount > 0) {
				for (auto& id : Registry::PackRegistry::GetSingleton().FindByTags(static_cast<size_t>(a_actorCount), tags)) {
					result.emplace_back(id);
				}
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

		// Current source file path playing on a_actor (as the caller passed it),
		// or "" if the actor has no live graph.
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

		// Pin akActor's solo graph to a WORLD point + heading (degrees). aiRootMode:
		// 0 pin / 1 additive / 2 follow. Also moves the capsule
		// there. Refused for scene participants (their placement is scene-driven).
		bool SetAnchor(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor,
			float a_x, float a_y, float a_z, float a_headingDeg, int32_t a_rootMode)
		{
			if (!a_actor) {
				REX::WARN("OSF.SetAnchor: no actor given");
				return false;
			}
			return Animation::GraphManager::GetSingleton().SetAnchor(a_actor, a_x, a_y, a_z, a_headingDeg, a_rootMode);
		}

		// Releases akActor's anchor — the graph returns to "follow" (rides the
		// actor's live transform). No-op if no anchor was set.
		bool ClearAnchor(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			if (!a_actor) {
				return false;
			}
			return Animation::GraphManager::GetSingleton().ClearAnchor(a_actor);
		}

		// Put N already-playing graphs on one shared clock (frame-lock). Call
		// OSF.Play on each actor first, then OSF.Sync. Scene participants are
		// skipped; needs >= 2 playable graphs.
		bool Sync(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors)
		{
			return Animation::GraphManager::GetSingleton().Sync(a_actors);
		}

		// Solo multi-phase sequence (primitive). Parallel arrays, equal length:
		// asFiles[i] phase clip, aiLoops[i] loops before advancing, afBlends[i]
		// blend-in secs. abLoopWhole restarts at phase 0 after the last.
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

		// Stop a live scene by its handle (from a Start* call). False if the handle is
		// invalid/ended. Fires NODE_EXIT + SCENE_END to registered callbacks.
		bool StopScene(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			return Scene::SceneRuntime::GetSingleton().Stop(a_scene);
		}

		// Actor convenience: stop the live scene a_actor participates in. Tries the SceneRuntime
		// handle first (fires NODE_EXIT + SCENE_END, invalidates it); falls back to a raw
		// GraphManager scene with no handle (a PlaySequence solo sequence). False if in neither.
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

		// DEBUG: replaces the player-lock input-disable masks. The flag names aren't fully
		// confirmed, so this lets you bisect the real bit layout in one session. 0/0 disables
		// nothing.
		void SetSceneControlMask(OSFVM&, uint32_t, std::monostate, int32_t a_userMask, int32_t a_otherMask)
		{
			Player::PlayerControlService::GetSingleton().SetMasks(
				static_cast<uint32_t>(a_userMask), static_cast<uint32_t>(a_otherMask));
		}

		// Compatibility-only natives (bound on OSFCompat). The SAF shim's non-Scene Play+Sync
		// path freezes the player via these standalone locks; the core never applies them on
		// its own. See OSFCompat.psc.

		// Standalone control lock: input-disable layer + AI-driven. false releases.
		void SetPlayerControlLock(OSFVM&, uint32_t, std::monostate, bool a_locked)
		{
			Player::PlayerControlService::GetSingleton().SetStandaloneLock(a_locked);
		}

		// Standalone camera lock: force/hold third person (bounces on zoom-in).
		// false restores the prior POV.
		void SetPlayerCameraLock(OSFVM&, uint32_t, std::monostate, bool a_locked)
		{
			Camera::CameraService::GetSingleton().SetStandaloneLock(a_locked);
		}

		// Engine crosshair target: the reference under the reticle / activate prompt.
		// Reads PlayerCharacter->commandTarget. Any ref kind (actor/door/container/...), or
		// null when the crosshair is on nothing.
		RE::TESObjectREFR* CrosshairTarget()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			return player ? player->commandTarget : nullptr;
		}

		// COMPATIBILITY-ONLY: the raw engine crosshair reference, or None. Restores
		// SAF's native crosshairRef the pure-Papyrus shim had no way to read.
		RE::TESObjectREFR* GetCrosshairRef(OSFVM&, uint32_t, std::monostate)
		{
			return CrosshairTarget();
		}

		// COMPATIBILITY-ONLY: the crosshair reference cast to Actor, or None when the
		// crosshair is on nothing or a non-actor ref (kACHR form-type gate). Backs the
		// SAF shim's crosshair pickers, which otherwise approximate selection with a
		// pure-Papyrus heading-angle cone search.
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

		// Save-safety fallback: drop scene/graph state anchored in the discarded
		// world. WARNING: only on an actual load — against a LIVE scene it leaves
		// participants animation-driven (StopAll skips actor restore). Use StopScene
		// for normal teardown.
		void NotifyGameLoaded(OSFVM&, uint32_t, std::monostate)
		{
			Animation::GraphManager::GetSingleton().StopAll("game loaded");
		}

		// True once OSF is loaded and initialized (playback hooks installed). The
		// optional-dependency gate consumers branch on (a SAF shim's Ping forwards here).
		bool IsReady(OSFVM&, uint32_t, std::monostate)
		{
			return Animation::GraphManager::GetSingleton().HooksInstalled();
		}

		// Whether a named feature is effective in this build. The lean core has ONE
		// gate (both playback hooks installed), so scenes/playback/sync/anchor all
		// report that single aggregate state; any other name => false.
		bool HasFeature(OSFVM&, uint32_t, std::monostate, RE::BSFixedString a_feature)
		{
			const std::string f = Util::ToLower(a_feature.c_str());
			// One aggregate gate: every engine-layer capability reports the same "are the
			// playback hooks installed + verified on this game build" state (they self-disable
			// together on a version mismatch). The scene-runtime capabilities (cues/actions/
			// sound/camera/callbacks) are part of the same merged engine.
			if (f == "scenes" || f == "playback" || f == "sync" || f == "anchor" ||
				f == "cues" || f == "actions" || f == "sound" || f == "camera" || f == "callbacks") {
				return Animation::GraphManager::GetSingleton().HooksInstalled();
			}
			return false;
		}

		// Start a scene by id, returning an opaque scene HANDLE (0 = failed). Routes through
		// SceneRuntime so callbacks fire and the handle drives GetSceneId/Node/StopScene/etc.
		// ID resolution: a `scene:` prefix forces the scene registry, `anim:` forces the
		// pack registry; a bare id resolves the scene registry first (a composed *.scene.json
		// graph), then the pack registry (a linear pack auto-exposed as a single-path scene).
		// aiStage = pack start stage (ignored for def-backed graphs).
		int32_t StartScene(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors, RE::BSFixedString a_id,
			int32_t a_stage)
		{
			if (a_actors.empty()) {
				REX::WARN("OSF.StartScene: no actors given");
				return 0;
			}
			std::string sid = a_id.c_str();
			bool forceScene = false;
			bool forcePack = false;
			if (sid.rfind("scene:", 0) == 0) {
				forceScene = true;
				sid = sid.substr(6);
			} else if (sid.rfind("anim:", 0) == 0) {
				forcePack = true;
				sid = sid.substr(5);
			}

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

		// Start a def-backed scene binding actors to NAMED roles: asRoles[i] is the role for
		// akActors[i] (equal lengths). Returns the handle (0 = no such scene / validation fail).
		// Roles are a *.scene.json concept; a `scene:` prefix is tolerated/stripped.
		int32_t StartSceneRoles(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			RE::BSFixedString a_id, std::vector<RE::BSFixedString> a_roles, int32_t /*a_stage*/)
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

		// Matchmake a registry pack by tags + gender slots and start it as a single-path scene.
		// Returns the scene handle (0 = no match / start failed). The chosen id is recoverable
		// via GetSceneId(handle).
		int32_t StartSceneByTags(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_tags)
		{
			if (a_actors.empty()) {
				REX::WARN("OSF.StartSceneByTags: no actors given");
				return 0;
			}
			for (auto* actor : a_actors) {
				if (!actor) {
					REX::WARN("OSF.StartSceneByTags: null actor in list");
					return 0;
				}
			}
			const auto genders = ActorGenders(a_actors);
			std::vector<std::string> tags;
			tags.reserve(a_tags.size());
			for (const auto& tag : a_tags) {
				tags.emplace_back(tag.c_str());
			}
			auto pick = Registry::PackRegistry::GetSingleton().PickByTags(tags, genders);
			if (!pick) {
				return 0;
			}
			std::vector<RE::Actor*> ordered(a_actors.size());
			for (size_t slot = 0; slot < pick->order.size(); slot++) {
				ordered[slot] = a_actors[pick->order[slot]];
			}
			const int32_t handle = Scene::SceneRuntime::GetSingleton().StartFromPack(pick->id, ordered, 0);
			if (handle) {
				REX::INFO("OSF.StartSceneByTags: playing '{}' (handle {:#010x})", pick->id, handle);
			}
			return handle;
		}

		// Ad-hoc one-shot scene from raw files (the SAF PlaySceneSeparate replacement):
		// co-locates the actors at actor[0], plays each file at afSpeed with afBlendIn, and
		// syncs the clock. Returns the scene handle (0 = failed).
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

		// --- Scene-event callbacks (Var[] payload) --------------------------------
		// Register akReceiver.asFn(Var[]) for events in aiEventMask (& scene aiScene, 0 =
		// any). Returns a generational token (0 = failed). Decode the Var[] via OSFEvent.
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

		// DEBUG (OSFCompat, off the public surface): synthesize one scene event and
		// dispatch it through the real relay to every registered receiver. Exercises the
		// registry + method-call path once a scripted form has registered.
		void Dbg_FireSceneEvent(OSFVM&, uint32_t, std::monostate, int32_t a_scene, int32_t a_event, RE::BSFixedString a_node)
		{
			REX::INFO("OSFCompat.Dbg_FireSceneEvent: scene={} event={:#x} node='{}' (-> relay)", a_scene, a_event, a_node.c_str());
			Scene::SceneEvent e;
			e.scene = a_scene;
			e.event = a_event;
			e.node = a_node.c_str();
			e.anchor = (a_event == Scene::Event::kNodeEnter) ? "enter" : (a_event == Scene::Event::kNodeExit) ? "exit" : "";
			Scene::SceneEventRelay::GetSingleton().Dispatch(e);
		}

		// DEBUG: lets a Papyrus receiver echo into OSF Animation.log (REX), so the
		// transport round-trip is provable without enabling the Papyrus script log.
		void Dbg_Log(OSFVM&, uint32_t, std::monostate, RE::BSFixedString a_msg)
		{
			REX::INFO("[Papyrus] {}", a_msg.c_str());
		}

		// DEBUG (OSFCompat): no-instance transport probe — DispatchStaticCall
		// asScript.asFn(Var[]) directly (no registration). Proves the Var[] marshalling
		// from the console without a scripted form.
		void Dbg_FireSceneEventStatic(OSFVM&, uint32_t, std::monostate, RE::BSFixedString a_script,
			RE::BSFixedString a_fn, int32_t a_scene, int32_t a_event, RE::BSFixedString a_node)
		{
			REX::INFO("OSFCompat.Dbg_FireSceneEventStatic: -> {}.{}(Var[]) scene={} event={:#x} node='{}'",
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

		// Start a scene from its *.scene.json def (entering at the def's entry node). DEBUG
		// helper on OSFCompat. Returns the handle (0 = fail).
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
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneRoles", &StartSceneRoles, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneByTags", &StartSceneByTags, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneFiles", &StartSceneFiles, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "FindScenes", &FindScenes, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "IsReady", &IsReady, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "HasFeature", &HasFeature, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "RegisterSceneCallback", &RegisterSceneCallback, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "UnregisterSceneCallback", &UnregisterSceneCallback, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneId", &GetSceneId, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneNode", &GetSceneNode, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneForActor", &GetSceneForActor, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneLoadErrors", &GetSceneLoadErrors, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "ValidateScene", &ValidateScene, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneValidationErrors", &GetSceneValidationErrors, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "AdvanceScene", &AdvanceScene, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "NavigateScene", &NavigateScene, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneEdgeCount", &GetSceneEdgeCount, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneEdgeId", &GetSceneEdgeId, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneEdgeLabel", &GetSceneEdgeLabel, true, false);
		REX::INFO("Registered papyrus natives on script '{}'", SCRIPT_NAME);

		// Compatibility-only natives — kept off the public OSF surface (see
		// COMPAT_SCRIPT_NAME / OSFCompat.psc). Only the SAF->OSF shim calls these.
		// SetSceneControlMask is a debug bisect tool parked here (not on OSF) so the
		// never-remove ABI promise doesn't lock in a throwaway native.
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "SetPlayerControlLock", &SetPlayerControlLock, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "SetPlayerCameraLock", &SetPlayerCameraLock, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "SetSceneControlMask", &SetSceneControlMask, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "GetCrosshairRef", &GetCrosshairRef, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "GetCrosshairActor", &GetCrosshairActor, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "Dbg_FireSceneEvent", &Dbg_FireSceneEvent, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "Dbg_FireSceneEventStatic", &Dbg_FireSceneEventStatic, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "Dbg_FireActionActor", &Dbg_FireActionActor, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "Dbg_StartScene", &Dbg_StartScene, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "Dbg_SetSceneNode", &Dbg_SetSceneNode, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "Dbg_StopScene", &Dbg_StopScene, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "Dbg_DumpScene", &Dbg_DumpScene, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "Dbg_StartSceneDef", &Dbg_StartSceneDef, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "Dbg_Log", &Dbg_Log, true, false);
		REX::INFO("Registered compatibility natives on script '{}'", COMPAT_SCRIPT_NAME);
	}

	bool RegisterFunctions()
	{
		// Mirrors main.cpp's kPostDataLoad grab so there is a single place that
		// knows how to fetch the live VM and (re)bind. Returns false if the VM is
		// not available yet so callers can log their own context.
		if (auto* gameVM = RE::GameVM::GetSingleton(); gameVM && gameVM->GetVM()) {
			RegisterFunctions(gameVM->GetVM());
			return true;
		}
		return false;
	}
}
