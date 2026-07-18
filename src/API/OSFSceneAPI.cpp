#include "API/OSFSceneAPI.h"

#include "API/SceneAPIControl.h"
#include "Animation/GraphManager.h"
#include "Animation/Scene.h"  // Animation::ScenePlan (ad-hoc files start)
#include "Registry/SceneRegistry.h"  // SceneDef::RequiresAnchor (anchor-bound guard)
#include "Scene/AnchorResolve.h"     // shared furniture-anchor resolution
#include "Scene/SceneRuntime.h"
#include "Util/StringUtil.h"  // Util::ToLower

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace OSF::API
{
	namespace
	{
		// Mirror of the canonical Scene::MakeOverrides/MakeAnchor sanitization (src/Scene/SceneLauncher.h — kept in lockstep).
		constexpr float kLoopScaleMax = 20.0f;

		// Tri-state: 1 = force on, 0 = force off, anything else (incl. -1) = inherit.
		std::optional<bool> TriState(std::int32_t a_v)
		{
			if (a_v == 1) {
				return true;
			}
			if (a_v == 0) {
				return false;
			}
			return std::nullopt;
		}

		Scene::SceneRuntime::StartOverrides MakeOverrides(const OSFStartOptions& a_opts)
		{
			Scene::SceneRuntime::StartOverrides over{};
			over.strip = TriState(a_opts.stripMode);
			over.lockPlayer = TriState(a_opts.lockPlayerMode);
			over.playerControl = TriState(a_opts.playerControlMode);
			over.fade = TriState(a_opts.fadeMode);
			// camera is a fixed buffer; read it bounded so a non-NUL-terminated POD can't overrun.
			if (const std::size_t len = ::strnlen(a_opts.camera, sizeof(a_opts.camera)); len > 0) {
				over.camera = std::string(a_opts.camera, len);
			}
			// Sanitize LoopScale HERE (never trust the POD): <=0 / NaN -> 1.0; overshoot -> clamp.
			float ls = a_opts.loopScale;
			if (!(ls > 0.0f)) {
				ls = 1.0f;
			} else if (ls > kLoopScaleMax) {
				ls = kLoopScaleMax;
			}
			over.loopScale = ls;
			// APPENDED fields (MINOR >= 2): read only when the consumer's stamped size covers them —
			// an older consumer's smaller POD ends before this field.
			if (a_opts.size >= offsetof(OSFStartOptions, inPlaceMode) + sizeof(a_opts.inPlaceMode)) {
				over.inPlace = TriState(a_opts.inPlaceMode);
			}
			return over;
		}

		Scene::SceneRuntime::AnchorOverride MakeAnchor(const OSFStartOptions& a_opts)
		{
			Scene::SceneRuntime::AnchorOverride anchor{};
			if (a_opts.hasAnchor) {
				anchor.set = true;
				anchor.pos = RE::NiPoint3{ a_opts.anchorX, a_opts.anchorY, a_opts.anchorZ };
				anchor.heading = a_opts.anchorHeadingRad;  // RADIANS (the POD documents this)
			}
			return anchor;
		}

		bool IsGltfPath(std::string_view a_path)
		{
			const auto ext = Util::ToLower(std::filesystem::path{ std::string(a_path) }.extension().string());
			return ext == ".glb" || ext == ".gltf";
		}

		// Split "File.glb:animId" into (path, animId), matching OSFScript.cpp's SplitRuntimeClipSpec.
		std::pair<std::string, std::string> SplitRuntimeClipSpec(std::string a_spec)
		{
			const auto pos = a_spec.rfind(':');
			if (pos == std::string::npos || pos + 1 >= a_spec.size()) {
				return { std::move(a_spec), {} };
			}
			std::string pathPart = a_spec.substr(0, pos);
			if (!IsGltfPath(pathPart)) {
				return { std::move(a_spec), {} };  // a bare drive-letter colon etc. — not a clip spec
			}
			std::string animId = a_spec.substr(pos + 1);
			return { std::move(pathPart), std::move(animId) };
		}

		// Copy the raw actor array into a vector; false (and a_out cleared) if empty or any actor is null
		bool CollectActors(RE::Actor* const* a_actors, std::uint32_t a_count, std::vector<RE::Actor*>& a_out)
		{
			a_out.clear();
			if (!a_actors || a_count == 0) {
				return false;
			}
			a_out.reserve(a_count);
			for (std::uint32_t i = 0; i < a_count; i++) {
				if (!a_actors[i]) {
					a_out.clear();
					return false;
				}
				a_out.push_back(a_actors[i]);
			}
			return true;
		}

		// An anchorbound scene with no anchorref cant be placed.
		bool AnchorBoundRefused(std::string_view a_sceneId, const char* a_tag)
		{
			const auto* def = Registry::SceneRegistry::GetSingleton().Find(a_sceneId);
			if (def && def->RequiresAnchor()) {
				REX::WARN("[API] {} '{}' is anchor-bound (furniture) - set OSFStartOptions.anchorRef to the furniture ref. Start refused", a_tag, a_sceneId);
				return true;
			}
			return false;
		}

		// Per-start speed for a DEF scene (startStage is resolved to an entry node BEFORE the start now —
		// see ResolveStartStageNode — so the scene enters on the stage instead of jumping after entry).
		void ApplyDefPostStart(std::int32_t a_handle, const std::vector<RE::Actor*>& a_actors, const OSFStartOptions& a_opts)
		{
			if (!a_handle) {
				return;
			}
			if (a_opts.speed != 1.0f && !a_actors.empty()) {
				Animation::GraphManager::GetSingleton().SetSpeed(a_actors.front(), a_opts.speed);
			}
		}

		// OSFStartOptions.startStage (> 0) -> the def's linear-stage entry node. LENIENT on purpose (C ABI):
		// an invalid stage warns and enters at the scene's own entry — the same effective behavior as the
		// old post-start jump, where SetStage on a bad stage silently no-opped. "" = the scene's own entry.
		std::string ResolveStartStageNode(std::string_view a_sceneId, std::int32_t a_stage, const char* a_tag)
		{
			if (a_stage <= 0) {
				return {};
			}
			const auto* def = Registry::SceneRegistry::GetSingleton().Find(a_sceneId);
			if (!def) {
				return {};  // unknown scene — the start itself will fail with the right log
			}
			if (def->linearStages.empty() || a_stage >= static_cast<std::int32_t>(def->linearStages.size())) {
				REX::WARN("[API] {} '{}': startStage {} invalid ({} linear stages) — entering at the scene entry",
					a_tag, def->id, a_stage, def->linearStages.size());
				return {};
			}
			return def->linearStages[a_stage];
		}

		// --- POD layout guard: locks the ABI of OSFStartOptions at OSF build time. A field add/reorder/resize that would silently break a shipped consumer copy fails HERE.
		// Appended fields are allowed only at the end (bump kOSFSceneAPIVersion MINOR + the size).
		static_assert(std::is_standard_layout_v<OSFStartOptions>, "OSFStartOptions must be standard-layout (POD ABI)");
		static_assert(sizeof(OSFStartOptions) == 136, "OSFStartOptions layout changed — update the size + version, ship a new header");
		static_assert(offsetof(OSFStartOptions, size) == 0, "OSFStartOptions.size must be first (cbSize prefix)");
		static_assert(offsetof(OSFStartOptions, camera) == 20, "OSFStartOptions.camera offset drift");
		static_assert(offsetof(OSFStartOptions, startStage) == 96, "OSFStartOptions.startStage offset drift");
		static_assert(offsetof(OSFStartOptions, anchorHeadingRad) == 116, "OSFStartOptions.anchorHeadingRad offset drift");
		static_assert(offsetof(OSFStartOptions, anchorRef) == 120, "OSFStartOptions.anchorRef offset drift");
		static_assert(offsetof(OSFStartOptions, inPlaceMode) == 128, "OSFStartOptions.inPlaceMode offset drift (MINOR 2 append)");
		static_assert(sizeof(OSFStartOptions::camera) == 64, "OSFStartOptions.camera buffer size changed");
	}

	// The concrete API singleton. Thin shims only
	class SceneAPIImpl final : public IOSFSceneAPI
	{
	public:
		static SceneAPIImpl& GetSingleton()
		{
			static SceneAPIImpl singleton;
			return singleton;
		}

		void          MarkReady() { _ready.store(true, std::memory_order_release); }
		IOSFSceneAPI* IfReady() { return _ready.load(std::memory_order_acquire) ? this : nullptr; }

		std::uint32_t GetInterfaceVersion() override { return kOSFSceneAPIVersion; }

		void GetPluginVersion(std::uint32_t& a_major, std::uint32_t& a_minor, std::uint32_t& a_patch) override
		{
			const auto v = SFSE::GetPluginVersion();
			a_major = v.major();
			a_minor = v.minor();
			a_patch = v.patch();
		}

		bool IsReady() override { return Animation::GraphManager::GetSingleton().HooksInstalled(); }

		std::int32_t StartScene(RE::Actor* const* a_actors, std::uint32_t a_count, const char* a_sceneId,
			const OSFStartOptions& a_opts) override
		{
			std::vector<RE::Actor*> actors;
			if (!a_sceneId || !CollectActors(a_actors, a_count, actors)) {
				return 0;
			}
			const auto over = MakeOverrides(a_opts);
			const std::string entryNode = ResolveStartStageNode(a_sceneId, a_opts.startStage, "StartScene");
			auto&      rt = Scene::SceneRuntime::GetSingleton();
			std::int32_t handle = 0;
			if (RE::TESObjectREFR* ref = a_opts.anchorRef) {
				// Furniture / explicit-ref anchoring: validate (anchor-bound) + compose the def's offset, using the ref's own facing. Shared with the Papyrus path.
				const auto resolved = Scene::ResolveSceneAnchor(a_sceneId, ref, std::nullopt, /*a_emitHud*/ false);
				if (!resolved) {
					return 0;  // anchor-bound scene, missing/incompatible furniture (logged in ResolveSceneAnchor)
				}
				handle = rt.StartFromDefAt(a_sceneId, actors, resolved->pos, resolved->heading, over, entryNode);
			} else {
				// No furniture ref: an anchor-bound scene can't be placed; otherwise free start with the optional raw WORLD anchor (hasAnchor) or co-located at actor[0].
				if (AnchorBoundRefused(a_sceneId, "StartScene")) {
					return 0;
				}
				const auto anchor = MakeAnchor(a_opts);
				handle = anchor.set
					? rt.StartFromDefAt(a_sceneId, actors, anchor.pos, anchor.heading, over, entryNode)
					: rt.StartFromDef(a_sceneId, actors, over, entryNode);
			}
			ApplyDefPostStart(handle, actors, a_opts);
			return handle;
		}

		std::int32_t StartSceneRoles(RE::Actor* const* a_actors, std::uint32_t a_count, const char* a_sceneId,
			const char* const* a_roles, std::uint32_t a_roleCount, const OSFStartOptions& a_opts) override
		{
			std::vector<RE::Actor*> actors;
			if (!a_sceneId || !CollectActors(a_actors, a_count, actors)) {
				return 0;
			}
			if (!a_roles || a_roleCount != a_count) {
				return 0;  // StartFromDefRoles requires one role per actor
			}
			// Resolve the anchor: a furniture ref (validated + composed) wins; else the raw world anchor, after refusing an anchor-bound scene that has no ref.
			Scene::SceneRuntime::AnchorOverride anchor{};
			if (RE::TESObjectREFR* ref = a_opts.anchorRef) {
				const auto resolved = Scene::ResolveSceneAnchor(a_sceneId, ref, std::nullopt, /*a_emitHud*/ false);
				if (!resolved) {
					return 0;  // anchor-bound scene, missing/incompatible furniture (logged)
				}
				anchor = *resolved;
			} else {
				if (AnchorBoundRefused(a_sceneId, "StartSceneRoles")) {
					return 0;
				}
				anchor = MakeAnchor(a_opts);
			}
			std::vector<std::string> roles;
			roles.reserve(a_roleCount);
			for (std::uint32_t i = 0; i < a_roleCount; i++) {
				roles.emplace_back(a_roles[i] ? a_roles[i] : "");
			}
			const std::string entryNode = ResolveStartStageNode(a_sceneId, a_opts.startStage, "StartSceneRoles");
			const std::int32_t handle = Scene::SceneRuntime::GetSingleton().StartFromDefRoles(
				a_sceneId, actors, roles, anchor, MakeOverrides(a_opts), entryNode);
			ApplyDefPostStart(handle, actors, a_opts);
			return handle;
		}

		std::int32_t StartSceneFiles(RE::Actor* const* a_actors, const char* const* a_files, std::uint32_t a_count,
			const OSFStartOptions& a_opts) override
		{
			std::vector<RE::Actor*> actors;
			if (!a_files || !CollectActors(a_actors, a_count, actors)) {
				return 0;
			}
			Animation::ScenePlan        plan;
			Animation::ScenePlan::Stage stage;
			stage.files.reserve(a_count);
			stage.animIds.reserve(a_count);
			for (std::uint32_t i = 0; i < a_count; i++) {
				auto [file, animId] = SplitRuntimeClipSpec(a_files[i] ? a_files[i] : "");
				stage.files.push_back(std::move(file));
				stage.animIds.push_back(std::move(animId));
			}
			stage.loops = 0;  // one synthetic holding stage
			plan.stages.push_back(std::move(stage));
			plan.speed = a_opts.speed;
			plan.blendIn = a_opts.blendIn;
			// Single-stage files scene: start at stage 0 (matches OSFAdvanced.StartSceneFiles);
			// OSFStartOptions.startStage is N/A for a one-stage hold.
			return Scene::SceneRuntime::GetSingleton().StartFromPlan(
				actors, std::move(plan), 0, MakeAnchor(a_opts), MakeOverrides(a_opts));
		}

		bool StopScene(std::int32_t a_handle) override
		{
			return Scene::SceneRuntime::GetSingleton().Stop(a_handle);
		}

		bool StopSceneForActor(RE::Actor* a_actor) override
		{
			if (!a_actor) {
				return false;
			}
			if (Scene::SceneRuntime::GetSingleton().StopForActor(a_actor)) {
				return true;
			}
			return Animation::GraphManager::GetSingleton().StopScene(a_actor);
		}

		bool SetStage(std::int32_t a_handle, std::int32_t a_stage) override
		{
			return Scene::SceneRuntime::GetSingleton().SetStage(a_handle, a_stage);
		}

		bool Advance(std::int32_t a_handle) override
		{
			return Scene::SceneRuntime::GetSingleton().Advance(a_handle);
		}

		bool Navigate(std::int32_t a_handle, const char* a_edgeId) override
		{
			return Scene::SceneRuntime::GetSingleton().Navigate(a_handle, a_edgeId ? a_edgeId : "");
		}

		bool Play(RE::Actor* a_actor, const char* a_file, const char* a_animId) override
		{
			if (!a_actor || !a_file) {
				return false;
			}
			return Animation::GraphManager::GetSingleton().PlayAnimation(a_actor, a_file, a_animId ? a_animId : "");
		}

		bool Stop(RE::Actor* a_actor) override
		{
			if (!a_actor) {
				return false;
			}
			return Animation::GraphManager::GetSingleton().StopAnimation(a_actor);
		}

		bool SetSpeed(RE::Actor* a_actor, float a_speed) override
		{
			if (!a_actor) {
				return false;
			}
			return Animation::GraphManager::GetSingleton().SetSpeed(a_actor, a_speed);
		}

		float GetSpeed(RE::Actor* a_actor) override
		{
			if (!a_actor) {
				return 0.0f;
			}
			return Animation::GraphManager::GetSingleton().GetSpeed(a_actor);
		}

		bool IsPlaying(RE::Actor* a_actor) override
		{
			if (!a_actor) {
				return false;
			}
			return Animation::GraphManager::GetSingleton().IsPlaying(a_actor);
		}

		std::int32_t GetSceneForActor(RE::Actor* a_actor) override
		{
			if (!a_actor) {
				return 0;
			}
			return Scene::SceneRuntime::GetSingleton().GetSceneForActor(a_actor);
		}

		std::int32_t GetSceneStage(std::int32_t a_handle) override
		{
			return Scene::SceneRuntime::GetSingleton().GetStage(a_handle);
		}

		std::uint32_t GetSceneParticipants(std::int32_t a_handle, RE::Actor** a_out, std::uint32_t a_cap) override
		{
			const auto          parts = Scene::SceneRuntime::GetSingleton().GetParticipants(a_handle);
			const std::uint32_t total = static_cast<std::uint32_t>(parts.size());
			if (a_out && a_cap) {
				const std::uint32_t n = std::min(total, a_cap);
				for (std::uint32_t i = 0; i < n; i++) {
					a_out[i] = parts[i];
				}
			}
			return total;
		}

	private:
		SceneAPIImpl() = default;

		std::atomic<bool> _ready{ false };
	};

	void MarkReady()
	{
		SceneAPIImpl::GetSingleton().MarkReady();
		REX::INFO("[API] native scene API ready (ABI {:#x})", kOSFSceneAPIVersion);
	}
}

// The exported factory. Stable, undecorated name "OSF_RequestSceneAPI" (extern "C").
// Returns nullptr on a MAJOR ABI mismatch or before OSF has marked the API ready.
extern "C" __declspec(dllexport) OSF::API::IOSFSceneAPI* OSF_RequestSceneAPI(std::uint32_t a_abiVersion)
{
	if ((a_abiVersion >> 16) != OSF::API::kOSFSceneAPIMajor) {
		return nullptr;  // incompatible vtable ABI - degrade rather than corrupt
	}
	return OSF::API::SceneAPIImpl::GetSingleton().IfReady();
}
