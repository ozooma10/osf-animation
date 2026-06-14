#include "OSFScript.h"

#include "Animation/GraphManager.h"
#include "Camera/CameraService.h"
#include "Player/PlayerControlService.h"
#include "Registry/PackRegistry.h"
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

		// Jumps the scene containing a_actor to the given stage (0-based).
		bool SetSceneStage(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor, int32_t a_stage)
		{
			if (!a_actor) {
				REX::WARN("OSF.SetSceneStage: no actor given");
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

		int32_t GetSceneStage(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
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
		// 0 pin / 1 additive / 2 follow (docs/ANCHORING.md). Also moves the capsule
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

		bool StopScene(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			if (!a_actor) {
				REX::WARN("OSF.StopScene: no actor given");
				return false;
			}
			return Animation::GraphManager::GetSingleton().StopScene(a_actor);
		}

		// DEBUG: replaces the player-lock input-disable masks (CLSF flag names
		// are unconfirmed — this bisects the real bit layout in one session).
		// 0/0 = disable nothing.
		void SetSceneControlMask(OSFVM&, uint32_t, std::monostate, int32_t a_userMask, int32_t a_otherMask)
		{
			Player::PlayerControlService::GetSingleton().SetMasks(
				static_cast<uint32_t>(a_userMask), static_cast<uint32_t>(a_otherMask));
		}

		// COMPATIBILITY-ONLY natives (bound on OSFCompat). The SAF shim's non-Scene
		// Play+Sync path freezes the player via these standalone locks (content-neutral
		// mechanism; the core never auto-applies them). See OSFCompat.psc.

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

		// Whether a named feature is effective in this build (playback self-disables
		// if the engine binding gates refused this game version). Unknown => false.
		bool HasFeature(OSFVM&, uint32_t, std::monostate, RE::BSFixedString a_feature)
		{
			const std::string f = Util::ToLower(a_feature.c_str());
			if (f == "scenes" || f == "playback" || f == "sync" || f == "anchor") {
				return Animation::GraphManager::GetSingleton().HooksInstalled();
			}
			return false;
		}

		// Registry scene by id (content-neutral: anchored, staged, synced — no
		// undress/voice/fade/camera policy; that is the OSF Intimacy layer).
		// 1-actor defs run as full 1-participant scenes.
		bool StartScene(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors, RE::BSFixedString a_id,
			int32_t a_stage)
		{
			if (a_actors.empty()) {
				REX::WARN("OSF.StartScene: no actors given");
				return false;
			}
			auto plan = Registry::PackRegistry::GetSingleton().BuildScenePlan(a_id.c_str(), a_actors.size());
			if (!plan) {
				return false;
			}
			return Animation::GraphManager::GetSingleton().PlaySceneStaged(a_actors, *plan, a_stage);
		}

		// Matchmake a registry scene by tags + gender slots, then start it.
		// Returns the chosen id, or "".
		RE::BSFixedString StartSceneByTags(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_tags)
		{
			if (a_actors.empty()) {
				REX::WARN("OSF.StartSceneByTags: no actors given");
				return "";
			}
			for (auto* actor : a_actors) {
				if (!actor) {
					REX::WARN("OSF.StartSceneByTags: null actor in list");
					return "";
				}
			}
			const auto genders = ActorGenders(a_actors);
			std::vector<std::string> tags;
			tags.reserve(a_tags.size());
			for (const auto& tag : a_tags) {
				tags.emplace_back(tag.c_str());
			}
			auto& registry = Registry::PackRegistry::GetSingleton();
			auto pick = registry.PickByTags(tags, genders);
			if (!pick) {
				return "";
			}
			std::vector<RE::Actor*> ordered(a_actors.size());
			for (size_t slot = 0; slot < pick->order.size(); slot++) {
				ordered[slot] = a_actors[pick->order[slot]];
			}
			auto plan = registry.BuildScenePlan(pick->id, ordered.size());
			bool ok = plan && Animation::GraphManager::GetSingleton().PlaySceneStaged(ordered, *plan, 0);
			if (ok) {
				REX::INFO("OSF.StartSceneByTags: playing '{}'", pick->id);
				return pick->id;
			}
			return "";
		}

		// Ad-hoc atomic scene from raw files (the SAF PlaySceneSeparate replacement):
		// co-locates the actors at actor[0], plays each file at afSpeed with afBlendIn,
		// and syncs the clock. Content-neutral — no policy.
		bool StartSceneFiles(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_files, float a_speed, float a_blendIn)
		{
			if (a_actors.size() != a_files.size()) {
				REX::WARN("OSF.StartSceneFiles: actor/file array sizes differ ({} vs {})", a_actors.size(), a_files.size());
				return false;
			}
			std::vector<std::string> files;
			files.reserve(a_files.size());
			for (const auto& f : a_files) {
				files.emplace_back(f.c_str());
			}
			Animation::ScenePlan plan;
			plan.stages.push_back({ files, {}, 0.0f });
			plan.speed = a_speed;
			plan.blendIn = a_blendIn;
			return Animation::GraphManager::GetSingleton().PlaySceneStaged(a_actors, plan, 0);
		}
	}

	void RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm)
	{
		a_vm->BindNativeMethod(SCRIPT_NAME, "Play", &Play, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "Stop", &Stop, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "SetSceneStage", &SetSceneStage, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "ReloadPacks", &ReloadPacks, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneStage", &GetSceneStage, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "IsPlaying", &IsPlaying, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetCurrentAnimation", &GetCurrentAnimation, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "SetSpeed", &SetSpeed, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSpeed", &GetSpeed, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "SetAnchor", &SetAnchor, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "ClearAnchor", &ClearAnchor, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "Sync", &Sync, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "PlaySequence", &PlaySequence, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StopScene", &StopScene, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "SetSceneControlMask", &SetSceneControlMask, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetVersion", &GetVersion, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "NotifyGameLoaded", &NotifyGameLoaded, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartScene", &StartScene, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneByTags", &StartSceneByTags, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneFiles", &StartSceneFiles, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "FindScenes", &FindScenes, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "IsReady", &IsReady, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "HasFeature", &HasFeature, true, false);
		REX::INFO("Registered papyrus natives on script '{}'", SCRIPT_NAME);

		// Compatibility-only natives — kept off the public OSF surface (see
		// COMPAT_SCRIPT_NAME / OSFCompat.psc). Only the SAF->OSF shim calls these.
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "SetPlayerControlLock", &SetPlayerControlLock, true, false);
		a_vm->BindNativeMethod(COMPAT_SCRIPT_NAME, "SetPlayerCameraLock", &SetPlayerCameraLock, true, false);
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
