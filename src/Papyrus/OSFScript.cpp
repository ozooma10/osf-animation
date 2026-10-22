#include "OSFScript.h"
#include "Profiling/Profile.h"

#include "API/MinimumVersion.h"
#include "API/BrowserForwarder.h"
#include "Animation/GraphManager.h"
#include "Animation/Scene.h"
#include "Audio/SoundService.h"
#include "Matchmaking/Matchmaker.h"
#include "Packs/PackReload.h"
#include "Registry/ContentRegistry.h"
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

			// Scan the small engine map case-insensitively instead of building a lowercase copy per call.
			const auto count = proxy->type->variables.size();
			const auto member = [&](const char* a_name) -> const RE::BSScript::Variable* {
				for (const auto& kv : proxy->type->varNameIndexMap) {
					if (Util::IEquals(kv.key.c_str(), a_name)) {
						return kv.value < count ? &proxy->variables[kv.value] : nullptr;
					}
				}
				return nullptr;
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
			if (const auto* v = member("HideApparelMode"); v && v->is<std::int32_t>()) {
				out.hideApparelMode = RE::BSScript::get<std::int32_t>(*v);
			}
			if (const auto* v = member("PlayerInputLockMode"); v && v->is<std::int32_t>()) {
				out.playerInputLockMode = RE::BSScript::get<std::int32_t>(*v);
			}
			if (const auto* v = member("SceneControlsMode"); v && v->is<std::int32_t>()) {
				out.sceneControlsMode = RE::BSScript::get<std::int32_t>(*v);
			}
			if (const auto* v = member("FadeMode"); v && v->is<std::int32_t>()) {
				out.fadeMode = RE::BSScript::get<std::int32_t>(*v);
			}
			if (const auto* v = member("FollowActorMode"); v && v->is<std::int32_t>()) {
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

		bool Play(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor, RE::BSFixedString a_file, RE::BSFixedString a_animId)
		{
			OSF_PROFILE_SCOPE("Papyrus.Play");
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
			OSF_PROFILE_SCOPE("Papyrus.Stop");
			if (!a_actor) {
				REX::DEBUG("[Papyrus] Stop: no actor given");
				return false;
			}
			return Animation::GraphManager::GetSingleton().StopAnimation(a_actor);
		}

		bool Sync(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors, bool a_anchor)
		{
			OSF_PROFILE_SCOPE("Papyrus.Sync");
			return Animation::GraphManager::GetSingleton().Sync(a_actors, a_anchor);
		}

		bool SetSceneStage(OSFVM&, uint32_t, std::monostate, int32_t a_scene, int32_t a_stage)
		{
			OSF_PROFILE_SCOPE("Papyrus.SetSceneStage");
			return Scene::SceneRuntime::GetSingleton().SetStage(a_scene, a_stage);
		}

		bool SetSceneStageForActor(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor, int32_t a_stage)
		{
			OSF_PROFILE_SCOPE("Papyrus.SetSceneStageForActor");
			if (!a_actor) {
				REX::DEBUG("[Papyrus] SetSceneStageForActor: no actor given");
				return false;
			}
			return Animation::GraphManager::GetSingleton().SetSceneStage(a_actor, a_stage);
		}

		int32_t ReloadPacks(OSFVM&, uint32_t, std::monostate)
		{
			OSF_PROFILE_SCOPE("Papyrus.ReloadPacks");
			return Content::ReloadAll();  // Papyrus name kept for compatibility
		}

		int32_t GetSceneStage(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			OSF_PROFILE_SCOPE("Papyrus.GetSceneStage");
			return Scene::SceneRuntime::GetSingleton().GetStage(a_scene);
		}

		int32_t GetSceneStageForActor(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			OSF_PROFILE_SCOPE("Papyrus.GetSceneStageForActor");
			return Animation::GraphManager::GetSingleton().GetSceneStage(a_actor);
		}

		bool IsPlaying(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			OSF_PROFILE_SCOPE("Papyrus.IsPlaying");
			return Animation::GraphManager::GetSingleton().IsPlaying(a_actor);
		}

		RE::BSFixedString GetCurrentAnimation(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			OSF_PROFILE_SCOPE("Papyrus.GetCurrentAnimation");
			if (!a_actor) {
				return "";
			}
			return RE::BSFixedString(Animation::GraphManager::GetSingleton().GetCurrentAnimation(a_actor).c_str());
		}

		bool SetSpeed(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor, float a_speed)
		{
			OSF_PROFILE_SCOPE("Papyrus.SetSpeed");
			if (!a_actor) {
				REX::DEBUG("[Papyrus] SetSpeed: no actor given");
				return false;
			}
			return Animation::GraphManager::GetSingleton().SetSpeed(a_actor, a_speed);
		}

		float GetSpeed(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			OSF_PROFILE_SCOPE("Papyrus.GetSpeed");
			if (!a_actor) {
				return 0.0f;
			}
			return Animation::GraphManager::GetSingleton().GetSpeed(a_actor);
		}

		bool SetSceneAlignment(OSFVM&, uint32_t, std::monostate, std::int32_t a_scene, RE::BSFixedString a_role,
			float a_x, float a_y, float a_z, float a_heading)
		{
			OSF_PROFILE_SCOPE("Papyrus.SetSceneAlignment");
			return Scene::SceneRuntime::GetSingleton().SetAlignment(a_scene, a_role.c_str(), { a_x, a_y, a_z, a_heading * Util::kDegToRadF });
		}
		std::vector<float> GetSceneAlignment(OSFVM&, uint32_t, std::monostate, std::int32_t a_scene, RE::BSFixedString a_role)
		{
			OSF_PROFILE_SCOPE("Papyrus.GetSceneAlignment");
			const auto value = Scene::SceneRuntime::GetSingleton().GetAlignment(a_scene, a_role.c_str());
			return value ? std::vector<float>{ value->x, value->y, value->z, value->heading / Util::kDegToRadF } : std::vector<float>{};
		}
		bool SaveSceneAlignment(OSFVM&, uint32_t, std::monostate, std::int32_t a_scene)
		{
			OSF_PROFILE_SCOPE("Papyrus.SaveSceneAlignment");
			return Scene::SceneRuntime::GetSingleton().SaveAlignment(a_scene);
		}
		bool ResetSceneAlignment(OSFVM&, uint32_t, std::monostate, std::int32_t a_scene, RE::BSFixedString a_role)
		{
			OSF_PROFILE_SCOPE("Papyrus.ResetSceneAlignment");
			return Scene::SceneRuntime::GetSingleton().SetAlignment(a_scene, a_role.c_str(), {});
		}

		bool SetSceneSpeed(OSFVM&, uint32_t, std::monostate, std::int32_t a_scene, float a_speed)
		{
			OSF_PROFILE_SCOPE("Papyrus.SetSceneSpeed");
			return Scene::SceneRuntime::GetSingleton().SetSpeed(a_scene, a_speed);
		}

		float GetSceneSpeed(OSFVM&, uint32_t, std::monostate, std::int32_t a_scene)
		{
			OSF_PROFILE_SCOPE("Papyrus.GetSceneSpeed");
			return Scene::SceneRuntime::GetSingleton().GetSpeed(a_scene);
		}

		bool SetAnchor(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor,
			float a_x, float a_y, float a_z, float a_headingDeg, int32_t a_rootMode)
		{
			OSF_PROFILE_SCOPE("Papyrus.SetAnchor");
			if (!a_actor) {
				REX::DEBUG("[Papyrus] SetAnchor: no actor given");
				return false;
			}
			return Animation::GraphManager::GetSingleton().SetAnchor(a_actor, a_x, a_y, a_z, a_headingDeg, a_rootMode);
		}

		bool ClearAnchor(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			OSF_PROFILE_SCOPE("Papyrus.ClearAnchor");
			if (!a_actor) {
				return false;
			}
			return Animation::GraphManager::GetSingleton().ClearAnchor(a_actor);
		}

		bool StopScene(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			OSF_PROFILE_SCOPE("Papyrus.StopScene");
			return Scene::SceneRuntime::GetSingleton().Stop(a_scene);
		}

		bool StopSceneForActor(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor)
		{
			OSF_PROFILE_SCOPE("Papyrus.StopSceneForActor");
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
			OSF_PROFILE_SCOPE("Papyrus.GetVersion");
			const auto v = SFSE::GetPluginVersion();
			return RE::BSFixedString(std::format("{}.{}.{}", v.major(), v.minor(), v.patch()));
		}

		bool ReportMinimumVersion(OSFVM&, uint32_t, std::monostate,
			RE::BSFixedString a_consumer, std::int32_t a_major,
			std::int32_t a_minor, std::int32_t a_patch)
		{
			OSF_PROFILE_SCOPE("Papyrus.ReportMinimumVersion");
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
			OSF_PROFILE_SCOPE("Papyrus.IsReady");
			return Animation::GraphManager::GetSingleton().HooksInstalled();
		}

		bool OpenBrowser(OSFVM&, uint32_t, std::monostate)
		{
			OSF_PROFILE_SCOPE("Papyrus.OpenBrowser");
			return API::OpenBrowser();
		}

		int32_t StartScene(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors, RE::BSFixedString a_id,
			SceneOptionsArg a_opts)
		{
			OSF_PROFILE_SCOPE("Papyrus.StartScene");
			return Scene::LaunchDef(a_id.c_str(), a_actors, Scene::MakeDefLaunchOptions(ReadSceneOptions(a_opts)),
				"[Papyrus] OSF.StartScene");
		}

		// Separate native: Papyrus structs can't hold the role array.
		bool ReplaceScene(OSFVM&, uint32_t, std::monostate, std::int32_t a_scene, RE::BSFixedString a_id, SceneOptionsArg a_opts)
		{
			OSF_PROFILE_SCOPE("Papyrus.ReplaceScene");
			const auto opts = ReadSceneOptions(a_opts);
			return Scene::SceneRuntime::GetSingleton().Replace(a_scene, a_id.c_str(), {},
				{ opts.anchor, Scene::OptHeadingRad(opts), opts.stage });
		}
		bool ReplaceSceneRoles(OSFVM&, uint32_t, std::monostate, std::int32_t a_scene,
			RE::BSFixedString a_id, std::vector<RE::BSFixedString> a_roles)
		{
			OSF_PROFILE_SCOPE("Papyrus.ReplaceSceneRoles");
			return Scene::SceneRuntime::GetSingleton().Replace(a_scene, a_id.c_str(), ToStrings(a_roles), {});
		}
		bool ReplaceSceneByTagsQuery(OSFVM&, uint32_t, std::monostate, std::int32_t a_scene,
			std::vector<RE::BSFixedString> a_allOf, std::vector<RE::BSFixedString> a_anyOf, std::vector<RE::BSFixedString> a_noneOf)
		{
			OSF_PROFILE_SCOPE("Papyrus.ReplaceSceneByTagsQuery");
			return Scene::SceneRuntime::GetSingleton().ReplaceByQuery(a_scene, { ToTags(a_allOf), ToTags(a_anyOf), ToTags(a_noneOf) });
		}
		// asRoles[i] constrains participant i (GetSceneParticipants order); an empty array = unconstrained.
		bool ReplaceSceneByQuery(OSFVM&, uint32_t, std::monostate, std::int32_t a_scene,
			std::vector<RE::BSFixedString> a_allOf, std::vector<RE::BSFixedString> a_anyOf, std::vector<RE::BSFixedString> a_noneOf,
			std::vector<RE::BSFixedString> a_roles, RE::TESObjectREFR* a_anchor, bool a_requireAnchor)
		{
			OSF_PROFILE_SCOPE("Papyrus.ReplaceSceneByQuery");
			if (a_requireAnchor && !a_anchor) {
				REX::DEBUG("[Papyrus] ReplaceSceneByQuery: abRequireAnchor needs akAnchor");
				return false;
			}
			return Scene::SceneRuntime::GetSingleton().ReplaceByQuery(a_scene, { ToTags(a_allOf), ToTags(a_anyOf), ToTags(a_noneOf) },
				ToStrings(a_roles), a_anchor, a_requireAnchor ? Matchmaking::AnchorMode::kRequire : Matchmaking::AnchorMode::kAllow);
		}

		RE::BSFixedString GetNode(OSFVM&, uint32_t, std::monostate, std::int32_t a_scene)
		{
			OSF_PROFILE_SCOPE("Papyrus.GetNode");
			return RE::BSFixedString(Scene::SceneRuntime::GetSingleton().GetNode(a_scene));
		}

		int32_t StartSceneRoles(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			RE::BSFixedString a_id, std::vector<RE::BSFixedString> a_roles)
		{
			OSF_PROFILE_SCOPE("Papyrus.StartSceneRoles");
			const auto roles = ToStrings(a_roles);
			return Scene::LaunchDef(a_id.c_str(), a_actors, {}, "[Papyrus] OSF.StartSceneRoles", &roles);
		}

		int32_t StartSceneByTags(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors, std::vector<RE::BSFixedString> a_tags, SceneOptionsArg a_opts)
		{
			OSF_PROFILE_SCOPE("Papyrus.StartSceneByTags");
			Matchmaking::TagQuery q;
			q.allOf = ToTags(a_tags);
			const auto opts = ReadSceneOptions(a_opts);
			return Scene::LaunchMatched(a_actors, q, opts, "[Papyrus] OSF.StartSceneByTags");
		}

		int32_t StartSceneByTagsQuery(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_allOf, std::vector<RE::BSFixedString> a_anyOf,
			std::vector<RE::BSFixedString> a_noneOf, SceneOptionsArg a_opts)
		{
			OSF_PROFILE_SCOPE("Papyrus.StartSceneByTagsQuery");
			Matchmaking::TagQuery q{ ToTags(a_allOf), ToTags(a_anyOf), ToTags(a_noneOf) };
			const auto opts = ReadSceneOptions(a_opts);
			return Scene::LaunchMatched(a_actors, q, opts, "[Papyrus] OSF.StartSceneByTagsQuery");
		}

		// Returns the matched scene id without starting it, or "" for no fit or bad input.
		RE::BSFixedString FindSceneByQuery(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_allOf, std::vector<RE::BSFixedString> a_anyOf, std::vector<RE::BSFixedString> a_noneOf,
			std::vector<RE::BSFixedString> a_roles, RE::TESObjectREFR* a_anchor, bool a_requireAnchor)
		{
			OSF_PROFILE_SCOPE("Papyrus.FindSceneByQuery");
			const auto roles = ToStrings(a_roles);
			if (a_actors.empty() || std::ranges::find(a_actors, nullptr) != a_actors.end() ||
				!Matchmaking::RoleConstraintsFit(roles, a_actors.size()) || (a_requireAnchor && !a_anchor)) {
				REX::DEBUG("[Papyrus] FindSceneByQuery: empty/null cast, role count mismatch, or abRequireAnchor without akAnchor");
				return RE::BSFixedString();
			}
			const auto pick = Matchmaking::Pick(a_actors, { ToTags(a_allOf), ToTags(a_anyOf), ToTags(a_noneOf) }, a_anchor,
				a_requireAnchor ? Matchmaking::AnchorMode::kRequire : Matchmaking::AnchorMode::kAllow, roles);
			return pick ? RE::BSFixedString(pick->id.c_str()) : RE::BSFixedString();
		}

		int32_t StartSceneByQuery(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_allOf, std::vector<RE::BSFixedString> a_anyOf, std::vector<RE::BSFixedString> a_noneOf,
			std::vector<RE::BSFixedString> a_roles, SceneOptionsArg a_opts, bool a_requireAnchor)
		{
			OSF_PROFILE_SCOPE("Papyrus.StartSceneByQuery");
			const auto opts = ReadSceneOptions(a_opts);
			if (a_requireAnchor && !opts.anchor) {
				REX::DEBUG("[Papyrus] StartSceneByQuery: abRequireAnchor needs SceneOptions.Anchor");
				return 0;
			}
			Matchmaking::TagQuery q{ ToTags(a_allOf), ToTags(a_anyOf), ToTags(a_noneOf) };
			return Scene::LaunchMatched(a_actors, q, ToStrings(a_roles), Scene::MakeDefLaunchOptions(opts),
				"[Papyrus] OSF.StartSceneByQuery",
				a_requireAnchor ? Matchmaking::AnchorMode::kRequire : Matchmaking::AnchorMode::kAllow);
		}

		int32_t StartSceneAtAnchor(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors, RE::TESObjectREFR* a_furniture, std::vector<RE::BSFixedString> a_tags, SceneOptionsArg a_opts)
		{
			OSF_PROFILE_SCOPE("Papyrus.StartSceneAtAnchor");
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
			return Scene::LaunchMatched(a_actors, q, opts, "[Papyrus] OSF.StartSceneAtAnchor", Matchmaking::AnchorMode::kRequire);
		}

		int32_t RegisterSceneCallback(OSFVM&, uint32_t, std::monostate, RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_fn, int32_t a_scene, int32_t a_eventMask)
		{
			OSF_PROFILE_SCOPE("Papyrus.RegisterSceneCallback");
			if (!a_receiver.get()) {
				REX::DEBUG("[Papyrus] RegisterSceneCallback: null receiver");
				return 0;
			}
			return Scene::SceneEventRelay::GetSingleton().Register(a_receiver, a_fn.c_str(), a_scene, a_eventMask);
		}

		int32_t RegisterSceneCallbackStatic(OSFVM&, uint32_t, std::monostate, RE::BSFixedString a_script, RE::BSFixedString a_fn, int32_t a_scene, int32_t a_eventMask)
		{
			OSF_PROFILE_SCOPE("Papyrus.RegisterSceneCallbackStatic");
			if (a_script.empty()) {
				REX::DEBUG("[Papyrus] RegisterSceneCallbackStatic: empty script name");
				return 0;
			}
			return Scene::SceneEventRelay::GetSingleton().RegisterStatic(a_script.c_str(), a_fn.c_str(), a_scene, a_eventMask);
		}

		bool UnregisterSceneCallback(OSFVM&, uint32_t, std::monostate, int32_t a_token)
		{
			OSF_PROFILE_SCOPE("Papyrus.UnregisterSceneCallback");
			return Scene::SceneEventRelay::GetSingleton().Unregister(a_token);
		}

		bool AdvanceScene(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			OSF_PROFILE_SCOPE("Papyrus.AdvanceScene");
			return Scene::SceneRuntime::GetSingleton().Advance(a_scene);
		}

		bool NavigateScene(OSFVM&, uint32_t, std::monostate, int32_t a_scene, RE::BSFixedString a_edgeId)
		{
			OSF_PROFILE_SCOPE("Papyrus.NavigateScene");
			return Scene::SceneRuntime::GetSingleton().Navigate(a_scene, a_edgeId.c_str());
		}

		int32_t GetSceneEdgeCount(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			OSF_PROFILE_SCOPE("Papyrus.GetSceneEdgeCount");
			return Scene::SceneRuntime::GetSingleton().EdgeCount(a_scene);
		}

		RE::BSFixedString GetSceneEdgeId(OSFVM&, uint32_t, std::monostate, int32_t a_scene, int32_t a_index)
		{
			OSF_PROFILE_SCOPE("Papyrus.GetSceneEdgeId");
			return RE::BSFixedString(Scene::SceneRuntime::GetSingleton().EdgeId(a_scene, a_index).c_str());
		}

		RE::BSFixedString GetSceneEdgeLabel(OSFVM&, uint32_t, std::monostate, int32_t a_scene, int32_t a_index)
		{
			OSF_PROFILE_SCOPE("Papyrus.GetSceneEdgeLabel");
			return RE::BSFixedString(Scene::SceneRuntime::GetSingleton().EdgeLabel(a_scene, a_index).c_str());
		}

		std::vector<RE::Actor*> GetSceneParticipants(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			OSF_PROFILE_SCOPE("Papyrus.GetSceneParticipants");
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
			OSF_PROFILE_SCOPE("Papyrus.StartSceneFiles");
			return StartFilesImpl("StartSceneFiles", a_actors, a_files, false, {}, {}, {}, {}, a_opts);
		}

		int32_t StartSceneFilesPlaced(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_files, std::vector<float> a_x, std::vector<float> a_y, std::vector<float> a_z,
			std::vector<float> a_headingDeg, SceneOptionsArg a_opts)
		{
			OSF_PROFILE_SCOPE("Papyrus.StartSceneFilesPlaced");
			return StartFilesImpl("StartSceneFilesPlaced", a_actors, a_files, true, a_x, a_y, a_z, a_headingDeg, a_opts);
		}

		int32_t StartSceneStages(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_files, std::vector<float> a_timers, std::vector<int32_t> a_loops,
			std::vector<float> a_blends, SceneOptionsArg a_opts)
		{
			OSF_PROFILE_SCOPE("Papyrus.StartSceneStages");
			return StartStagesImpl("StartSceneStages", a_actors, a_files, a_timers, a_loops, a_blends, false, {}, {}, {}, {}, a_opts);
		}

		int32_t StartSceneStagesPlaced(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			std::vector<RE::BSFixedString> a_files, std::vector<float> a_timers, std::vector<int32_t> a_loops,
			std::vector<float> a_blends, std::vector<float> a_x, std::vector<float> a_y, std::vector<float> a_z,
			std::vector<float> a_headingDeg, SceneOptionsArg a_opts)
		{
			OSF_PROFILE_SCOPE("Papyrus.StartSceneStagesPlaced");
			return StartStagesImpl("StartSceneStagesPlaced", a_actors, a_files, a_timers, a_loops, a_blends, true, a_x, a_y, a_z, a_headingDeg, a_opts);
		}

		int32_t StartSceneRolesEx(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors,
			RE::BSFixedString a_id, std::vector<RE::BSFixedString> a_roles, SceneOptionsArg a_opts)
		{
			OSF_PROFILE_SCOPE("Papyrus.StartSceneRolesEx");
			const auto roles = ToStrings(a_roles);
			return Scene::LaunchDef(a_id.c_str(), a_actors, Scene::MakeDefLaunchOptions(ReadSceneOptions(a_opts)),
				"[Papyrus] OSF.StartSceneRolesEx", &roles);
		}

		bool PlaySequence(OSFVM&, uint32_t, std::monostate, RE::Actor* a_actor,
			std::vector<RE::BSFixedString> a_files, std::vector<int32_t> a_loops, std::vector<float> a_blends, bool a_loopWhole)
		{
			OSF_PROFILE_SCOPE("Papyrus.PlaySequence");
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
			OSF_PROFILE_SCOPE("Papyrus.HideEquipment");
			return Scene::SceneRuntime::GetSingleton().HideEquipment(a_scene, a_actor, static_cast<std::uint32_t>(a_slotMask));
		}

		bool RestoreEquipment(OSFVM&, uint32_t, std::monostate, int32_t a_scene)
		{
			OSF_PROFILE_SCOPE("Papyrus.RestoreEquipment");
			return Scene::SceneRuntime::GetSingleton().RestoreHiddenEquipment(a_scene);
		}

		int32_t StopAllForActors(OSFVM&, uint32_t, std::monostate, std::vector<RE::Actor*> a_actors)
		{
			OSF_PROFILE_SCOPE("Papyrus.StopAllForActors");
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
			OSF_PROFILE_SCOPE("Papyrus.GetSceneLoadErrors");
			std::vector<RE::BSFixedString> out;
			for (const auto& e : Registry::ContentRegistry::GetSingleton().LoadErrors()) {
				out.emplace_back(e.c_str());
			}
			return out;
		}

		std::vector<RE::BSFixedString> GetMissingClipRefs(OSFVM&, uint32_t, std::monostate)
		{
			OSF_PROFILE_SCOPE("Papyrus.GetMissingClipRefs");
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
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetNode", &GetNode, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneByTags", &StartSceneByTags, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartScene", &StartScene, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneByTagsQuery", &StartSceneByTagsQuery, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneAtAnchor", &StartSceneAtAnchor, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneRoles", &StartSceneRoles, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "ReplaceScene", &ReplaceScene, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "ReplaceSceneRoles", &ReplaceSceneRoles, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "ReplaceSceneByTagsQuery", &ReplaceSceneByTagsQuery, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "ReplaceSceneByQuery", &ReplaceSceneByQuery, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "FindSceneByQuery", &FindSceneByQuery, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "StartSceneByQuery", &StartSceneByQuery, false, false);

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
		a_vm->BindNativeMethod(SCRIPT_NAME, "SetSceneSpeed", &SetSceneSpeed, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneSpeed", &GetSceneSpeed, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "SetSceneAlignment", &SetSceneAlignment, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "GetSceneAlignment", &GetSceneAlignment, true, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "SaveSceneAlignment", &SaveSceneAlignment, false, false);
		a_vm->BindNativeMethod(SCRIPT_NAME, "ResetSceneAlignment", &ResetSceneAlignment, false, false);
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
