#include "OSFScript.h"

#include "API/MinimumVersion.h"
#include "API/BrowserForwarder.h"
#include "Animation/GraphManager.h"
#include "Animation/Scene.h"
#include "Audio/SoundService.h"
#include "Matchmaking/Matchmaker.h"
#include "Packs/PackReload.h"
#include "Registry/SceneRegistry.h"
#include "Scene/AnchorResolve.h"
#include "Scene/SceneEventRelay.h"
#include "Scene/SceneLauncher.h"
#include "Scene/SceneRuntime.h"
#include "UI/HudMessage.h"
#include "Util/ClipPath.h"
#include "Util/Math.h"
#include "Util/StringUtil.h"

#include "RE/B/BSScriptUtil.h"
#include "RE/S/Struct.h"
#include "RE/S/StructTypeInfo.h"

#include <algorithm>
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

		// Lowercased (Matchmaker precondition); drops empty slots that would poison an allOf match.
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

		// Optional trailing `OSFTypes:SceneOptions akOpts = None` on the Start* natives.
		using SceneOptionsArg = std::optional<RE::BSScript::structure_wrapper<"OSFTypes", "SceneOptions">>;

		// Map members by iterating varNameIndexMap; don't trust structure_wrapper::find / varNameIndexMap.find.
		// Defaults live on Scene::LaunchOpts (mirrors OSFTypes.psc).
		Scene::LaunchOpts ReadSceneOptions(const SceneOptionsArg& a_opts)
		{
			Scene::LaunchOpts out;
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
				out.hideApparelMode = RE::BSScript::get<std::int32_t>(*v);
			}
			if (const auto* v = member("LockPlayerMode"); v && v->is<std::int32_t>()) {
				out.playerInputLockMode = RE::BSScript::get<std::int32_t>(*v);
			}
			if (const auto* v = member("PlayerControlMode"); v && v->is<std::int32_t>()) {
				out.sceneControlsMode = RE::BSScript::get<std::int32_t>(*v);
			}
			if (const auto* v = member("FadeMode"); v && v->is<std::int32_t>()) {
				out.fadeMode = RE::BSScript::get<std::int32_t>(*v);
			}
			if (const auto* v = member("InPlaceMode"); v && v->is<std::int32_t>()) {
				out.followActorMode = RE::BSScript::get<std::int32_t>(*v);
			}
			if (const auto* v = member("Camera"); v && v->is<RE::BSFixedString>()) {
				out.camera = RE::BSScript::get<RE::BSFixedString>(*v).c_str();
			}
			if (const auto* v = member("LoopScale"); v && v->is<float>()) {
				out.loopScale = RE::BSScript::get<float>(*v);
			}
			return out;
		}

		std::optional<Scene::SceneRuntime::AnchorOverride> ResolveSceneAnchor(const std::string& a_sceneId, const Scene::LaunchOpts& a_opts)
		{
			return Scene::ResolveSceneAnchor(a_sceneId, a_opts.anchor, Scene::OptHeadingRad(a_opts), /*a_emitHud*/ true);
		}

		// SceneOptions.Stage > 0 -> linear-stage entry node. nullopt = out of range (fail the start); "" = scene entry.
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

		bool Sync(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors, bool a_anchor)
		{
			return Animation::GraphManager::GetSingleton().Sync(a_actors, a_anchor);
		}

		bool SetSceneStage(OSFVM&, uint32_t, std::monostate, int32_t a_scene, int32_t a_stage)
		{
			return Scene::SceneRuntime::GetSingleton().SetStage(a_scene, a_stage);
		}

		bool SetSceneStageForActor(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor, int32_t a_stage)
		{
			if (!a_actor) {
				REX::DEBUG("[Papyrus] SetSceneStageForActor: no actor given");
				return false;
			}
			return Animation::GraphManager::GetSingleton().SetSceneStage(a_actor, a_stage);
		}

		int32_t ReloadPacks(OSFVM&, uint32_t, std::monostate)
		{
			return Content::ReloadAll();  // Papyrus name kept for compatibility
		}

		int32_t GetSceneStage(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			return Scene::SceneRuntime::GetSingleton().GetStage(a_scene);
		}

		int32_t GetSceneStageForActor(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			return Animation::GraphManager::GetSingleton().GetSceneStage(a_actor);
		}

		bool IsPlaying(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			return Animation::GraphManager::GetSingleton().IsPlaying(a_actor);
		}

		RE::BSFixedString GetCurrentAnimation(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			if (!a_actor) {
				return "";
			}
			return RE::BSFixedString(Animation::GraphManager::GetSingleton().GetCurrentAnimation(a_actor).c_str());
		}

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

		bool SetAnchor(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor,
			float a_x, float a_y, float a_z, float a_headingDeg, int32_t a_rootMode)
		{
			if (!a_actor) {
				REX::DEBUG("[Papyrus] SetAnchor: no actor given");
				return false;
			}
			return Animation::GraphManager::GetSingleton().SetAnchor(a_actor, a_x, a_y, a_z, a_headingDeg, a_rootMode);
		}

		bool ClearAnchor(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			if (!a_actor) {
				return false;
			}
			return Animation::GraphManager::GetSingleton().ClearAnchor(a_actor);
		}

		bool StopScene(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			return Scene::SceneRuntime::GetSingleton().Stop(a_scene);
		}

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

		RE::BSFixedString GetVersion(OSFVM&, uint32_t, std::monostate)
		{
			const auto v = SFSE::GetPluginVersion();
			return RE::BSFixedString(std::format("{}.{}.{}", v.major(), v.minor(), v.patch()));
		}

		bool ReportMinimumVersion(OSFVM&, uint32_t, std::monostate,
			RE::BSFixedString a_consumer, std::int32_t a_major,
			std::int32_t a_minor, std::int32_t a_patch)
		{
			if (a_major < 0 || a_minor < 0 || a_patch < 0) {
				REX::WARN("[Papyrus] ReportMinimumVersion rejected negative version component");
				return false;
			}
			return API::MinimumVersion::Report(a_consumer.c_str(),
				static_cast<std::uint32_t>(a_major), static_cast<std::uint32_t>(a_minor),
				static_cast<std::uint32_t>(a_patch)) == API::MinimumVersionResult::kSupported;
		}

		bool IsReady(OSFVM&, uint32_t, std::monostate)
		{
			return Animation::GraphManager::GetSingleton().HooksInstalled();
		}

		bool OpenBrowser(OSFVM&, uint32_t, std::monostate)
		{
			return API::OpenBrowser();
		}

		int32_t StartScene(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors, RE::BSFixedString a_id,
			SceneOptionsArg a_opts)
		{
			if (a_actors.empty()) {
				REX::DEBUG("[Papyrus] StartScene: no actors given");
				return 0;
			}
			const std::string sid = a_id.c_str();
			const auto def = Registry::ContentRegistry::GetSingleton().Find(sid);
			if (!def) {
				// ERROR, not DEBUG: a typo'd scene id must be visible at the shipped log level.
				REX::ERROR("[Papyrus] StartScene: no scene '{}'", sid);
				return 0;
			}
			const auto opts = ReadSceneOptions(a_opts);
			const auto entryNode = ResolveStageEntryNode(*def, opts.stage, "OSF.StartScene");
			if (!entryNode) {
				return 0;
			}
			const auto anchor = ResolveSceneAnchor(sid, opts);  // enforces anchor-bound furniture
			if (!anchor) {
				return 0;  // logged in ResolveSceneAnchor
			}
			const auto over = Scene::MakeOverrides(opts);
			auto& rt = Scene::SceneRuntime::GetSingleton();
			if (anchor->set) {
				return rt.StartFromDefAt(sid, a_actors, anchor->pos, anchor->heading, over, *entryNode);
			}
			return rt.StartFromDef(sid, a_actors, over, *entryNode);
		}

		// Separate native: Papyrus structs can't hold the role array.
		int32_t StartSceneRoles(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			RE::BSFixedString a_id, std::vector<RE::BSFixedString> a_roles)
		{
			if (a_actors.empty()) {
				REX::DEBUG("[Papyrus] StartSceneRoles: no actors given");
				return 0;
			}
			const std::string sid = a_id.c_str();
			// No SceneOptions here, so an anchor-bound scene can't be placed; point the caller at StartScene.
			if (const auto def = Registry::ContentRegistry::GetSingleton().Find(sid); def && def->RequiresAnchor()) {
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

		int32_t StartSceneByTags(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors, std::vector<RE::BSFixedString> a_tags, SceneOptionsArg a_opts)
		{
			Matchmaking::TagQuery q;
			q.allOf = ToTags(a_tags);
			const auto opts = ReadSceneOptions(a_opts);
			const auto over = Scene::MakeOverrides(opts);
			return Scene::LaunchMatched(a_actors, q, opts, over, "[Papyrus] OSF.StartSceneByTags");
		}

		int32_t StartSceneByTagsQuery(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_allOf, std::vector<RE::BSFixedString> a_anyOf,
			std::vector<RE::BSFixedString> a_noneOf, SceneOptionsArg a_opts)
		{
			Matchmaking::TagQuery q{ ToTags(a_allOf), ToTags(a_anyOf), ToTags(a_noneOf) };
			const auto opts = ReadSceneOptions(a_opts);
			const auto over = Scene::MakeOverrides(opts);
			return Scene::LaunchMatched(a_actors, q, opts, over, "[Papyrus] OSF.StartSceneByTagsQuery");
		}

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
			opts.anchor = a_furniture;  // drives both the matchmaking filter and the compose
			const auto over = Scene::MakeOverrides(opts);
			return Scene::LaunchMatched(a_actors, q, opts, over, "[Papyrus] OSF.StartSceneAtAnchor", Matchmaking::AnchorMode::kRequire);
		}

		int32_t RegisterSceneCallback(OSFVM&, uint32_t, std::monostate, RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_fn, int32_t a_scene, int32_t a_eventMask)
		{
			if (!a_receiver.get()) {
				REX::DEBUG("[Papyrus] RegisterSceneCallback: null receiver");
				return 0;
			}
			return Scene::SceneEventRelay::GetSingleton().Register(a_receiver, a_fn.c_str(), a_scene, a_eventMask);
		}

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

		std::vector<RE::Actor*> GetSceneParticipants(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			return Scene::SceneRuntime::GetSingleton().GetParticipants(a_scene);
		}

		Animation::PlaybackPlan::Segment MakeStageFromFiles(const std::vector<RE::BSFixedString>& a_files, std::size_t a_begin, std::size_t a_count)
		{
			Animation::PlaybackPlan::Segment stage;
			stage.files.reserve(a_count);
			stage.animIds.reserve(a_count);
			for (std::size_t i = 0; i < a_count; i++) {
				auto [file, animId] = Util::SplitRuntimeClipSpec(a_files[a_begin + i].c_str());
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

		// Shared by the plain and *Placed ad-hoc natives; a_tag names the caller in logs.
		int32_t StartFilesImpl(const char* a_tag, const std::vector<RE::Actor*>& a_actors,
			const std::vector<RE::BSFixedString>& a_files, bool a_placed, const std::vector<float>& a_x,
			const std::vector<float>& a_y, const std::vector<float>& a_z, const std::vector<float>& a_headingDeg,
			const SceneOptionsArg& a_opts)
		{
			if (a_actors.empty() || a_actors.size() != a_files.size()) {
				REX::DEBUG("[Papyrus] {}: actor/file count mismatch ({}/{})", a_tag, a_actors.size(), a_files.size());
				return 0;
			}
			if (a_placed && !PlacementArraysValid(a_tag, a_actors.size(), a_files.size(), a_x, a_y, a_z, a_headingDeg)) {
				return 0;
			}
			const auto opts = ReadSceneOptions(a_opts);
			Animation::PlaybackPlan plan;
			auto stage = MakeStageFromFiles(a_files, 0, a_files.size());
			if (a_placed) {
				stage.placements = MakePlacements(a_actors.size(), 0, a_x, a_y, a_z, a_headingDeg);
			}
			stage.loops = 0;
			plan.stages.push_back(std::move(stage));
			plan.speed = opts.speed;
			plan.blendIn = opts.blendIn;
			return Scene::SceneRuntime::GetSingleton().StartFromPlan(a_actors, std::move(plan), 0, Scene::MakeAnchor(opts), Scene::MakeOverrides(opts));
		}

		int32_t StartStagesImpl(const char* a_tag, const std::vector<RE::Actor*>& a_actors,
			const std::vector<RE::BSFixedString>& a_files, const std::vector<float>& a_timers,
			const std::vector<int32_t>& a_loops, const std::vector<float>& a_blends, bool a_placed,
			const std::vector<float>& a_x, const std::vector<float>& a_y, const std::vector<float>& a_z,
			const std::vector<float>& a_headingDeg, const SceneOptionsArg& a_opts)
		{
			if (a_actors.empty() || a_files.empty() || (a_files.size() % a_actors.size()) != 0) {
				REX::DEBUG("[Papyrus] {}: files must be stage-major and divisible by actor count ({}/{})", a_tag, a_files.size(), a_actors.size());
				return 0;
			}
			const std::size_t stageCount = a_files.size() / a_actors.size();
			const auto validLen = [stageCount](std::size_t n) { return n == 0 || n == stageCount; };
			if (!validLen(a_timers.size()) || !validLen(a_loops.size()) || !validLen(a_blends.size())) {
				REX::DEBUG("[Papyrus] {}: timers/loops/blends must be empty or length {}", a_tag, stageCount);
				return 0;
			}
			if (a_placed && !PlacementArraysValid(a_tag, a_actors.size(), a_files.size(), a_x, a_y, a_z, a_headingDeg)) {
				return 0;
			}
			const auto opts = ReadSceneOptions(a_opts);
			Animation::PlaybackPlan plan;
			plan.speed = opts.speed;
			plan.blendIn = opts.blendIn;
			plan.stages.reserve(stageCount);
			const bool timingGiven = !a_timers.empty() || !a_loops.empty();
			for (std::size_t s = 0; s < stageCount; s++) {
				auto stage = MakeStageFromFiles(a_files, s * a_actors.size(), a_actors.size());
				if (a_placed) {
					stage.placements = MakePlacements(a_actors.size(), s, a_x, a_y, a_z, a_headingDeg);
				}
				stage.timer = a_timers.empty() ? 0.0f : a_timers[s];
				stage.loops = a_loops.empty() ? (timingGiven ? 0 : 1) : a_loops[s];
				stage.blendIn = a_blends.empty() ? opts.blendIn : a_blends[s];
				plan.stages.push_back(std::move(stage));
			}
			return Scene::SceneRuntime::GetSingleton().StartFromPlan(a_actors, std::move(plan), opts.stage, Scene::MakeAnchor(opts), Scene::MakeOverrides(opts));
		}

		int32_t StartSceneFiles(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_files, SceneOptionsArg a_opts)
		{
			return StartFilesImpl("StartSceneFiles", a_actors, a_files, false, {}, {}, {}, {}, a_opts);
		}

		int32_t StartSceneFilesPlaced(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_files, std::vector<float> a_x, std::vector<float> a_y, std::vector<float> a_z,
			std::vector<float> a_headingDeg, SceneOptionsArg a_opts)
		{
			return StartFilesImpl("StartSceneFilesPlaced", a_actors, a_files, true, a_x, a_y, a_z, a_headingDeg, a_opts);
		}

		int32_t StartSceneStages(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_files, std::vector<float> a_timers, std::vector<int32_t> a_loops,
			std::vector<float> a_blends, SceneOptionsArg a_opts)
		{
			return StartStagesImpl("StartSceneStages", a_actors, a_files, a_timers, a_loops, a_blends, false, {}, {}, {}, {}, a_opts);
		}

		int32_t StartSceneStagesPlaced(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_files, std::vector<float> a_timers, std::vector<int32_t> a_loops,
			std::vector<float> a_blends, std::vector<float> a_x, std::vector<float> a_y, std::vector<float> a_z,
			std::vector<float> a_headingDeg, SceneOptionsArg a_opts)
		{
			return StartStagesImpl("StartSceneStagesPlaced", a_actors, a_files, a_timers, a_loops, a_blends, true, a_x, a_y, a_z, a_headingDeg, a_opts);
		}

		int32_t StartSceneRolesEx(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			RE::BSFixedString a_id, std::vector<RE::BSFixedString> a_roles, SceneOptionsArg a_opts)
		{
			if (a_actors.empty()) {
				REX::DEBUG("[Papyrus] StartSceneRolesEx: no actors given");
				return 0;
			}
			const auto opts = ReadSceneOptions(a_opts);
			const std::string sid = a_id.c_str();
			// SceneOptions.Stage enters directly on the stage node, as in StartScene.
			std::string entryNode;
			if (const auto def = Registry::ContentRegistry::GetSingleton().Find(sid)) {
				auto resolved = ResolveStageEntryNode(*def, opts.stage, "OSF.StartSceneRolesEx");
				if (!resolved) {
					return 0;
				}
				entryNode = std::move(*resolved);
			}
			// Same anchor pass as StartScene: enforces anchor-bound furniture and applies the authored anchorOffset.
			const auto anchor = ResolveSceneAnchor(sid, opts);
			if (!anchor) {
				return 0;  // logged in ResolveSceneAnchor
			}
			return Scene::SceneRuntime::GetSingleton().StartFromDefRoles(
				sid, a_actors, ToStrings(a_roles), *anchor, Scene::MakeOverrides(opts), entryNode);
		}

		bool PlaySequence(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor,
			std::vector<RE::BSFixedString> a_files, std::vector<int32_t> a_loops, std::vector<float> a_blends, bool a_loopWhole)
		{
			std::vector<std::string> files;
			std::vector<std::string> animIds;
			files.reserve(a_files.size());
			animIds.reserve(a_files.size());
			for (const auto& spec : a_files) {
				auto [file, animId] = Util::SplitRuntimeClipSpec(spec.c_str());
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
			for (const auto& e : Registry::ContentRegistry::GetSingleton().LoadErrors()) {
				out.emplace_back(e.c_str());
			}
			return out;
		}

		std::vector<RE::BSFixedString> GetMissingClipRefs(OSFVM&, uint32_t, std::monostate)
		{
			std::vector<RE::BSFixedString> out;
			for (const auto& e : Registry::ContentRegistry::GetSingleton().MissingClipRefs()) {
				out.emplace_back(e.c_str());
			}
			return out;
		}
	}

	void RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm)
	{
		// Preload OSFTypes so its structs exist before struct-typed natives bind and before the relay's CreateStruct.
		{
			RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> typesType;
			if (!a_vm->GetScriptObjectType(RE::BSFixedString(TYPES_SCRIPT_NAME.data()), typesType) || !typesType) {
				REX::ERROR("[Papyrus] could not preload '{}' type info — struct-typed natives "
						  "(StartScene/StartSceneByTags/StartSceneByTagsQuery) may fail to register and "
						  "scene-event callbacks may receive no payload", TYPES_SCRIPT_NAME);
			}
		}

		// a_taskletCallable=false on every engine-mutating native so the VM runs it on the game thread
		// (equipment/weapon/fade/HUD services are game-thread only). Pure readers stay tasklet-callable.
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneByTags", &StartSceneByTags, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartScene", &StartScene, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneByTagsQuery", &StartSceneByTagsQuery, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneAtAnchor", &StartSceneAtAnchor, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneRoles", &StartSceneRoles, false, false);

		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "StartSceneFiles", &StartSceneFiles, false, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "StartSceneFilesPlaced", &StartSceneFilesPlaced, false, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "StartSceneRolesEx", &StartSceneRolesEx, false, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "StartSceneStages", &StartSceneStages, false, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "StartSceneStagesPlaced", &StartSceneStagesPlaced, false, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "PlaySequence", &PlaySequence, false, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "HideEquipment", &HideEquipment, false, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "RestoreEquipment", &RestoreEquipment, false, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "StopAllForActors", &StopAllForActors, false, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "GetSceneLoadErrors", &GetSceneLoadErrors, true, false);
		a_vm->BindNativeMethod(ADVANCED_SCRIPT_NAME, "GetMissingClipRefs", &GetMissingClipRefs, true, false);

		a_vm->BindNativeMethod(SCRIPT_NAME, "RegisterSceneCallback", &RegisterSceneCallback, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "RegisterSceneCallbackStatic", &RegisterSceneCallbackStatic, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "UnregisterSceneCallback", &UnregisterSceneCallback, true, false);

		a_vm->BindNativeMethod(SCRIPT_NAME, "AdvanceScene", &AdvanceScene, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "NavigateScene", &NavigateScene, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneEdgeCount", &GetSceneEdgeCount, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneEdgeId", &GetSceneEdgeId, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneEdgeLabel", &GetSceneEdgeLabel, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneParticipants", &GetSceneParticipants, true, false);

		a_vm->BindNativeMethod(SCRIPT_NAME, "IsPlaying", &IsPlaying, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "Play", &Play, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "Stop", &Stop, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "Sync", &Sync, false, false);

		a_vm->BindNativeMethod(SCRIPT_NAME, "SetSpeed", &SetSpeed, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSpeed", &GetSpeed, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "SetAnchor", &SetAnchor, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "ClearAnchor", &ClearAnchor, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetCurrentAnimation", &GetCurrentAnimation, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "SetSceneStage", &SetSceneStage, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneStage", &GetSceneStage, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "SetSceneStageForActor", &SetSceneStageForActor, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneStageForActor", &GetSceneStageForActor, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StopScene", &StopScene, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StopSceneForActor", &StopSceneForActor, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "ReloadPacks", &ReloadPacks, false, false);

		a_vm->BindNativeMethod(SCRIPT_NAME, "IsReady", &IsReady, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetVersion", &GetVersion, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "OpenBrowser", &OpenBrowser, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "ReportMinimumVersion", &ReportMinimumVersion, true, false);
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
