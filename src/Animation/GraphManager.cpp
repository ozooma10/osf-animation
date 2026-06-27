#include "GraphManager.h"

#include "Audio/SoundService.h"
#include "Camera/CameraService.h"
#include "Input/InputService.h"
#include "Player/PlayerControlService.h"
#include "UI/FadeService.h"
#include "UI/Subtitle.h"
#include "Serialization/AFImport.h"
#include "Serialization/GLTFImport.h"
#include "Util/Ba2.h"
#include "Util/Math.h"
#include "Util/StringUtil.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <fstream>

namespace OSF::Animation
{
	namespace
	{
		// The two engine hooks. InstallHooks verifies each vtable slot still points where we expect before patching it.
		// AnimationManager::Update - slot 4, the clock/sampling point.
		// TODO: move to commonlib
		constexpr REL::ID AnimManagerVTableID(467920);
		constexpr REL::ID AnimManagerUpdateFnID(122232);
		constexpr size_t UpdateVFuncIdx = 4;
		
		// BGSModelNode::Update - slot 2, sig (modelNode, &fadeNode->local, NiUpdateData*).
		// Stamping before the original runs is the rig-buffer write point (that same call composes + commits).
		// TODO: move to commonlib
		constexpr REL::ID ModelNodeVTableID(400534);
		constexpr REL::ID ModelNodeUpdateFnID(48634);
		constexpr size_t ModelNodeUpdateVFuncIdx = 2;

		// CAP (not floor) for a pinned actor's cull-sphere radius (meters). The engine fades an actor out as the
		// camera nears it — the SAME near-fade that hides the player in 3rd person when you zoom in — and that
		// fade distance SCALES WITH THIS RADIUS. Inflating the sphere (the old max(...,2.5)) pushed the fade out
		// to ~2.5 m, so zooming the orbit in vanished the partner. We now CAP the radius small so the camera can
		// get close before any fade. Frustum safety comes from the CENTER (kept on the torso, where scene_orbit
		// always points), not from a big radius. (Proven in-game: 8.0 was strictly worse than 2.5.)
		constexpr float kPinCullRadius = 1.0f;

		// Lift the cull-sphere center from the compose-root (= the actor's feet/origin = pinWorld) up to the
		// torso, where the visible posed mesh actually sits. The engine's own capsule-derived bound is centered
		// near the torso; clobbering the center down to the feet mis-aims the sphere low, so at certain orbit
		// angles it leaves the view frustum and the participant pops out. Meters along world +Z.
		constexpr float kPinCullCenterUp = 1.0f;

		// BSFadeNode near-camera fade flag (+0x1B4, a float: 1.0 = drawn, 0.0 = faded).
		// The engine fades an actor out when the third-person camera orbits close; we hold it at 1.0 each frame so pinned participants don't vanish.
		constexpr std::ptrdiff_t kFadeNodeVisFlagOff = 0x1B4;

		// Resolve a pack clip spec to an absolute path: absolute passes through;
		// a  relative spec tries Data/<spec>, then Data/OSF/Animations/<filename>. 
		// Returns the primary Data path if neither exists, so the load-failure log names what the author wrote.
		std::filesystem::path ResolveClipPath(const std::filesystem::path& a_spec)
		{
			if (a_spec.is_absolute()) {
				return a_spec;
			}
			auto primary = std::filesystem::current_path() / "Data" / a_spec;
			std::error_code ec;
			if (std::filesystem::exists(primary, ec)) {
				return primary;
			}
			auto fallback = std::filesystem::current_path() / "Data" / "OSF" / "Animations" / a_spec.filename();
			if (std::filesystem::exists(fallback, ec)) {
				return fallback;
			}
			return primary;
		}

		// Raw bytes of the human skeleton.rig for the .af importer. Prefers a loose file (dev / extracted),
		// otherwise reads it straight out of the base game BA2 (Starfield - Animations.ba2) so OSF needs
		// no shipped vanilla asset. Runs once per session — AFImport caches the parsed rig + skeleton.
		std::optional<std::vector<std::uint8_t>> LoadHumanRigBytes()
		{
			const auto data = std::filesystem::current_path() / "Data";
			const std::filesystem::path looseCandidates[] = {
				data / "OSF" / "skeleton.rig",
				data / "OSF" / "Animations" / "skeleton.rig",
				data / "meshes" / "actors" / "human" / "characterassets" / "skeleton.rig",  // vanilla path, if extracted loose
			};
			std::error_code ec;
			for (const auto& cand : looseCandidates) {
				if (std::filesystem::exists(cand, ec)) {
					std::ifstream f(cand, std::ios::binary);
					if (f) {
						std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
						if (!bytes.empty()) {
							REX::TRACE("[Anim] rig: loose {}", cand.string());
							return bytes;
						}
					}
				}
			}
			if (auto b = Util::Ba2::ReadGameFile("meshes/actors/human/characterassets/skeleton.rig", "Starfield - Animations.ba2")) {
				REX::TRACE("[Anim] rig: read from BA2 ({} bytes)", b->size());
				return b;
			}
			REX::ERROR("[Anim] rig: no loose skeleton.rig and none found in any Data\\*.ba2");
			return std::nullopt;
		}

		struct ClipLoad
		{
			std::shared_ptr<const OzzSkeleton>  skeleton;
			std::shared_ptr<const OzzAnimation> anim;
			bool                                ok = false;
			std::string                         detail;
		};

		// Unified clip load for the ozz path: a `.af` goes through AFImport (engine-native clip decoded
		// against the shipped human rig); anything else (`.glb`/`.gltf`) through GLTFImport. Both yield
		// an ozz {skeleton, anim} the Graph sampler consumes identically.
		ClipLoad LoadClip(const std::filesystem::path& a_file, std::string_view a_animId)
		{
			ClipLoad out;
			if (Util::ToLower(a_file.extension().string()) == ".af") {
				auto r = Serialization::AFImport::LoadAnimation(a_file, "human-skeleton", &LoadHumanRigBytes);
				if (r.error != Serialization::AFError::kSuccess) {
					out.detail = std::format("af error {}: {}", static_cast<int>(r.error), r.detail);
					return out;
				}
				out.skeleton = std::move(r.skeleton);
				out.anim = std::move(r.anim);
				out.ok = true;
				return out;
			}
			auto r = Serialization::GLTFImport::LoadAnimation(a_file, a_animId);
			if (r.error != Serialization::GLTFError::kSuccess) {
				out.detail = std::format("gltf error {}: {}", static_cast<int>(r.error), r.detail);
				return out;
			}
			out.skeleton = std::move(r.skeleton);
			out.anim = std::move(r.anim);
			out.ok = true;
			return out;
		}

		bool IsGameMenuPaused()
		{
			if (auto* main = RE::Main::GetSingleton()) {
				return main->isGameMenuPaused;
			}
			return false;
		}

		//flag for if console is open.
		std::atomic<bool> g_consoleOpen{ false };

		class ConsoleMenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static ConsoleMenuSink* GetSingleton()
			{
				static ConsoleMenuSink instance;
				return &instance;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event,
				RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event.menuName == RE::Console::MENU_NAME) {
					g_consoleOpen.store(a_event.opening, std::memory_order_relaxed);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		float PlaybackDelta(const RE::BSAnimationUpdateData& a_updateData)
		{
			//Freeze playback if menu open (or console which doesnt set flag)
			if (IsGameMenuPaused() || g_consoleOpen.load(std::memory_order_relaxed)) {
				return 0.0f;
			}

			// Animation updates are normally subdivided into small slices. After a menu pause the engine can report accumulated wall time; 
			// that is a resume/catch-up spike for OSF playback, not animation time.
			constexpr float kMaxUpdateStep = 0.1f;
			const float dt = a_updateData.timeDelta;
			return (std::isfinite(dt) && dt >= 0.0f && dt <= kMaxUpdateStep) ? dt : 0.0f;
		}

		// Validates a play request's shapes (actor count, per-stage file/placement counts, no null actors); 
		// logs the specific failure. The hook-installed precondition is checked separately by the caller.
		bool ValidateScenePlanArgs(const std::vector<RE::Actor*>& a_actors, const ScenePlan& a_plan, int32_t a_startStage)
		{
			if (a_actors.empty()) {
				REX::ERROR("[Anim] PlayScene: need >= 1 actor (got 0)");
				return false;
			}
			if (a_plan.stages.empty()) {
				REX::ERROR("[Anim] PlayScene: plan has no stages");
				return false;
			}
			if (a_startStage < 0 || static_cast<size_t>(a_startStage) >= a_plan.stages.size()) {
				REX::ERROR("[Anim] PlayScene: start stage {} out of range (plan has {} stages)", a_startStage, a_plan.stages.size());
				return false;
			}
			for (size_t s = 0; s < a_plan.stages.size(); s++) {
				const auto& stage = a_plan.stages[s];
				if (stage.files.size() != a_actors.size() ||
					(!stage.placements.empty() && stage.placements.size() != a_actors.size())) {
					REX::ERROR("[Anim] PlayScene: stage {} does not match the actor count ({} files, {} placements, {} actors)",
						s, stage.files.size(), stage.placements.size(), a_actors.size());
					return false;
				}
			}
			for (size_t i = 0; i < a_actors.size(); i++) {
				if (!a_actors[i]) {
					REX::ERROR("[Anim] PlayScene: null actor at index {}", i);
					return false;
				}
			}
			return true;
		}

		// Loads every clip of every stage up front and assembles the Scene (stages, voices, sizing, initial stage, world anchor). 
		// Returns nullptr if any clip fails to load - the caller refuses a partial scene. Touches  no GraphManager state; the caller wires up the participant graphs.
		std::shared_ptr<Scene> BuildSceneFromPlan(const std::vector<RE::Actor*>& a_actors, const ScenePlan& a_plan,
			int32_t a_startStage)
		{
			auto scene = std::make_shared<Scene>();
			scene->stages.reserve(a_plan.stages.size());
			for (const auto& planStage : a_plan.stages) {
				Scene::StageData stage;
				stage.timer = planStage.timer;
				stage.loops = planStage.loops;
				stage.blendIn = (planStage.blendIn >= 0.0f) ? planStage.blendIn : a_plan.blendIn;
				stage.placements = planStage.placements.empty() ?
				                       std::vector<ParticipantPlacement>(a_actors.size()) :
				                       planStage.placements;
				stage.marks = planStage.marks;
				for (const auto& fileSpec : planStage.files) {
					auto file = ResolveClipPath(std::filesystem::path{ fileSpec });
					auto load = LoadClip(file, "");
					if (!load.ok) {
						REX::ERROR("[Anim] PlayScene: failed to load '{}' ({}) — scene not started",
							file.string(), load.detail);
						return nullptr;
					}
					stage.participants.push_back({ std::move(load.skeleton), std::move(load.anim), fileSpec });
				}
				stage.duration = stage.participants[0].anim->data->duration();
				scene->stages.push_back(std::move(stage));
			}
			scene->animId = a_plan.animId;
			scene->anchored = a_plan.anchored;
			scene->loopWhole = a_plan.loopWhole;
			scene->speed = std::clamp(a_plan.speed, 0.0f, 100.0f);  // same range as SetSpeed (no raw afSpeed)

			// Current-stage state: placements is sized once here and only ever mutated element-wise afterwards (the pin reads it lock-free).
			scene->placements.resize(a_actors.size());
			scene->SetStage(a_startStage);

			// Anchor at actor[0]'s current transform by default; participant world positions are anchor + each placement's offset rotated into the anchor heading frame (see PlacementToWorld).
			// An explicit anchor (StartSceneAt) overrides actor[0], so the scene world-anchors to a thing (furniture/marker).
			if (a_plan.anchorExplicit) {
				scene->anchorPos = a_plan.anchorPos;
				scene->anchorHeading = a_plan.anchorHeading;
			} else {
				scene->anchorPos = a_actors[0]->data.location;
				scene->anchorHeading = a_actors[0]->data.angle.z;
			}
			return scene;
		}
	}

	GraphManager& GraphManager::GetSingleton()
	{
		static GraphManager singleton;
		return singleton;
	}

	void GraphManager::InstallHooks()
	{
		REL::Relocation<uintptr_t> vtbl{ AnimManagerVTableID };
		REL::Relocation<uintptr_t> expected{ AnimManagerUpdateFnID };

		const uintptr_t slotValue = *reinterpret_cast<uintptr_t*>(vtbl.address() + UpdateVFuncIdx * sizeof(uintptr_t));
		if (slotValue != expected.address()) {
			REX::ERROR("[Anim] AnimationManager vtable slot {} = {:X}, expected AnimationManager::Update at {:X} — "
				"AddressLib IDs stale for this game version, NOT patching (animations disabled)",
				UpdateVFuncIdx, slotValue, expected.address());
			return;
		}

		REL::Relocation<uintptr_t> mnVtbl{ ModelNodeVTableID };
		REL::Relocation<uintptr_t> mnExpected{ ModelNodeUpdateFnID };

		const uintptr_t mnSlotValue = *reinterpret_cast<uintptr_t*>(mnVtbl.address() + ModelNodeUpdateVFuncIdx * sizeof(uintptr_t));
		if (mnSlotValue != mnExpected.address()) {
			REX::ERROR("[Anim] BGSModelNode vtable slot {} = {:X}, expected BGSModelNode::Update at {:X} — "
				"AddressLib IDs stale for this game version, NOT patching (animations disabled)",
				ModelNodeUpdateVFuncIdx, mnSlotValue, mnExpected.address());
			return;
		}

		_origAnimGraphUpdate = reinterpret_cast<AnimUpdateFn*>(
			vtbl.write_vfunc(UpdateVFuncIdx, &Hook_AnimGraphUpdate));
		_origModelNodeUpdate = reinterpret_cast<ModelNodeUpdateFn*>(
			mnVtbl.write_vfunc(ModelNodeUpdateVFuncIdx, &Hook_ModelNodeUpdate));
		REX::TRACE("[Anim] installed AnimationManager::Update (vfunc {}) + BGSModelNode::Update (vfunc {}) hooks",
			UpdateVFuncIdx, ModelNodeUpdateVFuncIdx);
	}

	void GraphManager::RegisterConsolePauseSink()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			REX::WARN("[Anim] console-pause sink not registered: RE::UI singleton null — the console will NOT freeze playback");
			return;
		}
		ui->RegisterSink<RE::MenuOpenCloseEvent>(ConsoleMenuSink::GetSingleton());
		REX::DEBUG("[Anim] registered console MenuOpenCloseEvent sink (freezes playback while the console is open)");
	}

	bool GraphManager::PlayAnimation(RE::Actor* a_actor, std::string_view a_file, std::string_view a_animId)
	{
		if (!a_actor) {
			return false;
		}

		if (!_origAnimGraphUpdate) {
			REX::ERROR("[Anim] Play refused: update hook is not installed");
			return false;
		}

		auto file = ResolveClipPath(std::filesystem::path{ a_file });

		auto loadResult = LoadClip(file, a_animId);
		if (!loadResult.ok) {
			REX::ERROR("[Anim] Failed to load animation '{}' ({})", file.string(), loadResult.detail);
			return false;
		}

		std::shared_ptr<Graph> g;
		{
			std::unique_lock l{ stateLock };
			auto& slot = graphs[a_actor];
			if (slot) {
				// Refuse to clobber a scene participant (it would keep g->scene set and the next stage tick would overwrite this clip anyway). 
				// Mirror of StopAnimation's guard — use StopScene to end a scene first.
				std::scoped_lock gl{ slot->stateLock };
				if (slot->scene) {
					REX::WARN("[Anim] Play refused: actor {:X} is in a scene — use StopScene first", a_actor->formID);
					return false;
				}
			} else {
				slot = std::make_shared<Graph>();
				slot->target.reset(a_actor);
			}
			g = slot;
			graphCount.store(graphs.size(), std::memory_order_relaxed);
		}

		{
			std::unique_lock gl{ g->stateLock };
			g->SetAnimation(loadResult.skeleton, loadResult.anim, std::string(a_file));
		}

		REX::DEBUG("[Anim] Playing '{}' on actor {:X}", file.filename().string(), a_actor->formID);
		return true;
	}

	bool GraphManager::StopAnimation(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}

		std::shared_lock l{ stateLock };
		auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) {
			return false;
		}
		std::scoped_lock gl{ iter->second->stateLock };
		if (iter->second->scene) {
			REX::WARN("[Anim] Stop: actor {:X} is in a scene — use StopScene", a_actor->formID);
			return false;
		}
		iter->second->BeginFadeOut();
		return true;
	}

	void GraphManager::RemoveFadedGraph(RE::TESObjectREFR* a_refr)
	{
		std::unique_lock l{ stateLock };
		auto iter = graphs.find(a_refr);
		if (iter == graphs.end()) {
			return;
		}
		auto g = iter->second;  // keep alive past the erase
		bool faded = false;
		{
			std::scoped_lock gl{ g->stateLock };
			faded = !g->scene && g->IsFadedOut();  // replayed meanwhile? keep it
		}
		if (faded) {
			graphs.erase(iter);
			graphCount.store(graphs.size(), std::memory_order_relaxed);
		}
	}

	bool GraphManager::PlaySceneStaged(const std::vector<RE::Actor*>& a_actors, const ScenePlan& a_plan, int32_t a_startStage)
	{
		if (!_origAnimGraphUpdate) {
			REX::ERROR("[Anim] PlayScene refused: update hook is not installed");
			return false;
		}
		if (!ValidateScenePlanArgs(a_actors, a_plan, a_startStage)) {
			return false;
		}

		// Load every clip of every stage up front; a null result means one failed, so refuse a partial scene. 
		// Preloading is what makes stage switches on the job threads IO-free.
		auto scene = BuildSceneFromPlan(a_actors, a_plan, a_startStage);
		if (!scene) {
			return false;
		}

		const auto startStage = static_cast<uint32_t>(a_startStage);
		{
			std::unique_lock l{ stateLock };

			// Re-playing on actors already in a scene: tear the old scene(s) down first so movement state is restored exactly once per scene.
			for (auto* actor : a_actors) {
				if (auto iter = graphs.find(actor); iter != graphs.end() && iter->second->scene) {
					REX::DEBUG("[Anim] PlayScene: actor {:X} already in a scene — stopping it first", actor->formID);
					StopSceneLocked(iter->second->scene);
				}
			}

			for (size_t i = 0; i < a_actors.size(); i++) {
				const auto& startSlot = scene->stages[startStage].participants[i];
				auto& slot = graphs[a_actors[i]];
				if (!slot) {
					slot = std::make_shared<Graph>();
					slot->target.reset(a_actors[i]);
				}
				{
					std::scoped_lock gl{ slot->stateLock };
					slot->SetAnimation(startSlot.skeleton, startSlot.anim, startSlot.file);
					slot->blendDuration = scene->stages[startStage].blendIn;
					slot->scene = scene.get();
					slot->participantIndex = static_cast<int>(i);
					slot->appliedStage = startStage;
					slot->sceneFrames = 0;    // restart the LogSceneDiag cadence for this scene
					slot->hasAnchor = false;  // the scene drives positioning; drop any solo SetAnchor
					slot->syncGroup.reset();  // the scene clock supersedes any Sync group

				}
				scene->participants.push_back(slot);
			}
			scenes.push_back(scene);
			graphCount.store(graphs.size(), std::memory_order_relaxed);
		}

		// Initial placement. One teleport holds - idle actors get no position write-back;
		// the compose-root pin in the BGSModelNode hook maintains the visual position each frame against physics capsule drift.
		// NOTE: AI off is NOT an option - the engine stops AnimationManager::Update for AI-disabled NPCs, which freezes playback.
		auto* transforms = RE::TransformService::GetSingleton();
		for (size_t i = 0; scene->anchored && i < scene->participants.size() && transforms; i++) {
			const auto& pl = scene->placements[i];
			const RE::NiPoint3 worldPos = PlacementToWorld(scene->anchorPos, scene->anchorHeading, pl);
			transforms->SetPosition(scene->participants[i]->target.get(), worldPos);
			transforms->SetHeadingZ(scene->participants[i]->target.get(), scene->anchorHeading + pl.heading);
			if (pl.x != 0.0f || pl.y != 0.0f || pl.z != 0.0f || pl.heading != 0.0f) {
				REX::TRACE("[Anim] placement[{}]: local ({:+.2f},{:+.2f},{:+.2f}) heading {:+.2f} -> world ({:.1f},{:.1f},{:.1f})",
					i, pl.x, pl.y, pl.z, pl.heading, worldPos.x, worldPos.y, worldPos.z);
			}
		}

		// Movement controller animation-driven so graph root motion is the only intentional position writer (the engine's own paired-scene anchor);
		// reverted in StopScene. Dispatched to the game thread via the SFSE task queue,  the switch never engaged when called from the Papyrus thread (bit 19 stayed clear); 
		// the engine's own callers (console, sync-anim manager) all run on the game thread. 
		// Only for anchored scenes - an unanchored (in-place / synced-in-formation) scene leaves movement engine-owned.
		if (scene->anchored) {
			for (auto* actor : a_actors) {
				RE::NiPointer<RE::Actor> keepAlive{ actor };
				SFSE::GetTaskInterface()->AddTask([keepAlive]() {
					auto* ctl = keepAlive->movementController;
					if (!ctl) {
						REX::WARN("[Anim] Actor {:X} has no movement controller — skipping animation-driven switch",
							keepAlive->formID);
						return;
					}
					ctl->SetAnimationDriven();
					REX::TRACE("[Anim] Actor {:X}: animation-driven requested (game thread, mode byte now {})",
						keepAlive->formID, ctl->movementMode);
				});
			}
		}

		REX::INFO("[Anim] Started synced scene: {} actors, {} stage(s) starting at {} (duration {:.2f}s, timer {:.1f}s, loops {}), "
			"anchored at ({:.1f},{:.1f},{:.1f}) heading {:.2f}",
			a_actors.size(), scene->stages.size(), startStage, scene->duration,
			scene->stages[startStage].timer, scene->stages[startStage].loops,
			scene->anchorPos.x, scene->anchorPos.y, scene->anchorPos.z, scene->anchorHeading);
		return true;
	}

	bool GraphManager::SetSceneStage(RE::Actor* a_actor, int32_t a_stage)
	{
		if (!a_actor) {
			return false;
		}

		// shared stateLock keeps the scene alive (scenes only mutate under unique);
		// the stage jump itself is guarded by the scene's own lock
		std::shared_lock l{ stateLock };
		auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) {
			return false;
		}
		Scene* scene = nullptr;
		{
			std::scoped_lock gl{ iter->second->stateLock };
			scene = iter->second->scene;
		}
		if (!scene) {
			REX::WARN("[Anim] SetSceneStage: actor {:X} is not in a scene", a_actor->formID);
			return false;
		}
		if (!scene->SetStage(a_stage)) {
			REX::WARN("[Anim] SetSceneStage: stage {} out of range ({} stages)", a_stage, scene->stages.size());
			return false;
		}
		REX::DEBUG("[Anim] Scene jumped to stage {}", a_stage);
		return true;
	}

	bool GraphManager::StopScene(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}

		std::unique_lock l{ stateLock };
		auto iter = graphs.find(a_actor);
		if (iter == graphs.end() || !iter->second->scene) {
			return false;
		}
		StopSceneLocked(iter->second->scene);
		return true;
	}

	void GraphManager::StopSceneByPtr(Scene* a_scene)
	{
		std::unique_lock l{ stateLock };
		StopSceneLocked(a_scene);  // no-op if the scene was already stopped
	}

	void GraphManager::StopAll(const char* a_reason)
	{
		// Release the player standalone lock + the persistent AI-driven flag (set by the scene runtime's control lock).
		// The AI-driven flag is serialized into saves, so it MUST be cleared on every load even when this process holds no in-memory lock;
		// the input-disable layer is non-persistent.
		Camera::CameraService::GetSingleton().OnStopAll();
		Player::PlayerControlService::GetSingleton().OnStopAll();
		// Drop any live director-input grant too (its scene is about to be cleared below).
		Input::InputService::GetSingleton().OnStopAll();

		// Release any held/pending screen fade before the load (the stay-faded latch crashes the load path).
		UI::FadeService::GetSingleton().OnStopAll();
		// Clear any subtitle still in the box so it can't bleed into the loaded world.
		UI::Subtitle::OnStopAll();
		// Cut every live cue sound, a loaded save shouldn't have last-world sounds ringing over it.
		Audio::SoundService::GetSingleton().StopAll();

		// Drop the scene runtime's handles too, their participants are raw Actor* that the load invalidates, so a stashed handle must read as dead afterward. 
		// Done even when we hold no graphs/scenes ourselves, so the handle table can never stay live across a load. 
		// Runs before stateLock, since Clear takes the runtime's own lock.
		if (_clearHandler) {
			_clearHandler();
		}

		std::unique_lock l{ stateLock };
		if (scenes.empty() && graphs.empty()) {
			return;
		}
		REX::INFO("[Anim] StopAll ({}): dropping {} scene(s) + {} graph(s)",
			a_reason ? a_reason : "?", scenes.size(), graphs.size());

		// We do NOT fade or revert movement here: on a save load the engine has already reset every actor to the saved state, and our graphs are anchored in the world that was just discarded. 
		// Stale graphs/scenes just stop existing; the engine's rig refresh owns the pose.
		scenes.clear();
		graphs.clear();
		graphCount.store(0, std::memory_order_relaxed);
	}

	int32_t GraphManager::GetSceneStage(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return -1;
		}
		std::shared_lock l{ stateLock };
		auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) {
			return -1;
		}
		Scene* scene = nullptr;
		{
			std::scoped_lock gl{ iter->second->stateLock };
			scene = iter->second->scene;
		}
		// Report the scene's authoritative current stage (set immediately on SetStage / auto-advance), not the graph's appliedStage (lags one sample). 
		// stateLock (shared) keeps the scene alive while we read it.
		return scene ? static_cast<int32_t>(scene->CurrentStage()) : -1;
	}

	bool GraphManager::IsPlaying(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		std::shared_lock l{ stateLock };
		auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) {
			return false;
		}
		std::scoped_lock gl{ iter->second->stateLock };
		return !iter->second->IsFadedOut();
	}

	std::string GraphManager::GetCurrentAnimation(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return {};
		}
		std::shared_lock l{ stateLock };
		auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) {
			return {};
		}
		std::scoped_lock gl{ iter->second->stateLock };
		if (iter->second->IsFadedOut()) {
			return {};
		}
		return iter->second->currentFile;
	}

	bool GraphManager::SetSpeed(RE::Actor* a_actor, float a_speed)
	{
		if (!a_actor) {
			return false;
		}
		const float speed = std::clamp(a_speed, 0.0f, 100.0f);  // 1.0 = authored, 0 = freeze
		std::shared_lock l{ stateLock };
		auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) {
			return false;
		}
		std::scoped_lock gl{ iter->second->stateLock };
		// Scene participants share the scene clock (Scene::Advance reads speed); 
		// a synced solo graph shares its group clock (which advances by the group speed, not the owner's); an unsynced solo graph advances by Graph::speed.
		if (iter->second->scene) {
			iter->second->scene->speed = speed;
		} else if (iter->second->syncGroup) {  // null only in the brief window before SetAnimation runs
			iter->second->syncGroup->speed.store(speed, std::memory_order_relaxed);
		}
		return true;
	}

	float GraphManager::GetSpeed(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return 0.0f;
		}
		std::shared_lock l{ stateLock };
		auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) {
			return 0.0f;
		}
		std::scoped_lock gl{ iter->second->stateLock };
		if (iter->second->scene) {
			return iter->second->scene->speed.load(std::memory_order_relaxed);
		}
		if (iter->second->syncGroup) {
			return iter->second->syncGroup->speed.load(std::memory_order_relaxed);
		}
		return 1.0f;  // graph created but not yet started (pre-SetAnimation), authored speed
	}

	bool GraphManager::SetAnchor(RE::Actor* a_actor, float a_x, float a_y, float a_z, float a_headingDeg, int32_t a_rootMode)
	{
		if (!a_actor) {
			return false;
		}
		const float heading = a_headingDeg * Util::kDegToRadF;
		const auto mode = static_cast<RootMode>(std::clamp(a_rootMode, 0, 2));
		{
			std::shared_lock l{ stateLock };
			auto iter = graphs.find(a_actor);
			if (iter == graphs.end()) {
				REX::WARN("[Anim] SetAnchor: actor {:X} has no live graph", a_actor->formID);
				return false;
			}
			std::scoped_lock gl{ iter->second->stateLock };
			if (iter->second->scene) {
				REX::WARN("[Anim] SetAnchor: actor {:X} is a scene participant — anchoring is scene-driven (use the scene's placement offsets)",
					a_actor->formID);
				return false;
			}
			iter->second->anchorPos = { a_x, a_y, a_z };
			iter->second->rootMode = mode;
			iter->second->hasAnchor = true;
		}
		// The pin only fixes the RENDERED root; move the capsule to the anchor too (keeps physics/interaction near) and set heading once. 
		// SetPosition runs fine from the calling thread (PlaySceneStaged does the same directly).
		if (auto* transforms = RE::TransformService::GetSingleton()) {
			transforms->SetPosition(a_actor, { a_x, a_y, a_z });
			transforms->SetHeadingZ(a_actor, heading);
		}
		REX::TRACE("[Anim] SetAnchor: actor {:X} -> ({:.1f},{:.1f},{:.1f}) heading {:.2f} rootMode {}",
			a_actor->formID, a_x, a_y, a_z, heading, a_rootMode);
		return true;
	}

	bool GraphManager::ClearAnchor(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		std::shared_lock l{ stateLock };
		auto iter = graphs.find(a_actor);
		if (iter == graphs.end()) {
			return false;
		}
		std::scoped_lock gl{ iter->second->stateLock };
		iter->second->hasAnchor = false;
		return true;
	}

	bool GraphManager::Sync(const std::vector<RE::Actor*>& a_actors, bool a_anchor)
	{
		if (a_actors.size() < 2) {
			REX::WARN("[Anim] Sync: need >= 2 actors (got {})", a_actors.size());
			return false;
		}

		// Auto-anchor (the default): promote the independently-played solo graphs into ONE anchored, clock-shared scene at actor[0]'s current transform. 
		// Placements are left empty (same-spot overlap), so each paired clip's baked root offset arranges the actors about one shared origin+heading
		// Without this, Play+Sync leaves every actor animating about its own (far-apart) world position, which is the "actors stand apart" symptom.
		if (a_anchor) {
			std::vector<RE::Actor*> sceneActors;
			std::vector<std::string> sceneFiles;
			float groupSpeed = 1.0f;
			bool gotSpeed = false;
			{
				// Collect the qualifying solo graphs' clips in actor order WITHOUT  mutating them, then drop stateLock before PlaySceneStaged (which takes it unique). 
				// Skip actors with no graph, no clip, or already in a scene.
				std::shared_lock l{ stateLock };
				for (auto* actor : a_actors) {
					if (!actor) {
						continue;
					}
					auto iter = graphs.find(actor);
					if (iter == graphs.end()) {
						REX::DEBUG("[Anim] Sync: actor {:X} has no live graph — skipping", actor->formID);
						continue;
					}
					std::scoped_lock gl{ iter->second->stateLock };
					if (iter->second->scene) {
						REX::DEBUG("[Anim] Sync: actor {:X} is already a scene participant — skipping", actor->formID);
						continue;
					}
					if (iter->second->currentFile.empty()) {
						REX::DEBUG("[Anim] Sync: actor {:X} has no clip to anchor — skipping", actor->formID);
						continue;
					}
					if (!gotSpeed) {  // carry a pre-Sync SetSpeed over to the scene clock
						groupSpeed = iter->second->syncGroup->speed;
						gotSpeed = true;
					}
					sceneActors.push_back(actor);
					sceneFiles.push_back(iter->second->currentFile);
				}
			}
			if (sceneActors.size() < 2) {
				REX::WARN("[Anim] Sync: fewer than 2 anchorable solo graphs ({}) — nothing synced "
						  "(pass abAnchor=false for in-place clock sync)", sceneActors.size());
				return false;
			}
			// One ad-hoc anchored stage; empty placements = same-spot overlap at actor[0].
			ScenePlan plan;
			plan.anchored = true;
			plan.speed = groupSpeed;
			ScenePlan::Stage stage;
			stage.files = std::move(sceneFiles);
			plan.stages.push_back(std::move(stage));
			if (!PlaySceneStaged(sceneActors, plan, 0)) {
				REX::WARN("[Anim] Sync: anchored promotion failed; actors left unsynced");
				return false;
			}
			REX::DEBUG("[Anim] Sync: {} graphs promoted to one anchored scene (same-spot overlap at actor {:X})",
				sceneActors.size(), sceneActors.front()->formID);
			return true;
		}

		// Opt-out (abAnchor=false): legacy clock-merge only, frame-lock the solo graphs on one shared clock, leaving each actor at its own world position.
		std::shared_lock l{ stateLock };

		// First pass: collect the qualifying solo graphs WITHOUT mutating them, so a group that can't reach 2 members leaves nothing half-synced 
		// (a stray 1-member assignment strands that graph on a private clock and snaps it to frame 0 even though we report failure). 
		// Holding stateLock shared across both passes blocks scene (un)assignment, which takes stateLock unique, so the scene check from pass 1 stays valid through pass 2.
		std::vector<std::shared_ptr<Graph>> targets;
		for (auto* actor : a_actors) {
			if (!actor) {
				continue;
			}
			auto iter = graphs.find(actor);
			if (iter == graphs.end()) {
				REX::DEBUG("[Anim] Sync: actor {:X} has no live graph — skipping", actor->formID);
				continue;
			}
			std::scoped_lock gl{ iter->second->stateLock };
			if (iter->second->scene) {
				REX::DEBUG("[Anim] Sync: actor {:X} is a scene participant (already clock-synced) — skipping", actor->formID);
				continue;
			}
			targets.push_back(iter->second);
		}
		if (targets.size() < 2) {
			REX::WARN("[Anim] Sync: fewer than 2 playable solo graphs to sync ({})", targets.size());
			return false;
		}
		// Second pass: one shared clock for the whole group; each graph reads it under its own lock (the group lock is a leaf). 
		// Graphs jump to the clock's t=0 on their next sample, so the group snaps into phase together. 
		// Seed the group speed from the first member so a pre-Sync SetSpeed carries over.
		auto group = std::make_shared<SyncGroup>();
		{
			std::scoped_lock gl{ targets.front()->stateLock };
			if (const auto& sg = targets.front()->syncGroup) {  // null only pre-SetAnimation
				group->speed.store(sg->speed.load(std::memory_order_relaxed), std::memory_order_relaxed);
			}
		}
		for (auto& g : targets) {
			std::scoped_lock gl{ g->stateLock };
			g->syncGroup = group;
		}
		REX::TRACE("[Anim] Sync: {} graphs frame-locked on one shared clock", targets.size());
		return true;
	}

	bool GraphManager::PlaySequence(RE::Actor* a_actor, const std::vector<std::string>& a_files, const std::vector<int32_t>& a_loops, const std::vector<float>& a_blends, bool a_loopWhole)
	{
		if (!a_actor) {
			return false;
		}
		if (a_files.empty() || a_files.size() != a_loops.size() || a_files.size() != a_blends.size()) {
			REX::WARN("[Anim] PlaySequence: files/loops/blends must be non-empty and equal length ({}/{}/{})",
				a_files.size(), a_loops.size(), a_blends.size());
			return false;
		}
		ScenePlan plan;
		plan.loopWhole = a_loopWhole;
		plan.anchored = false;           // primitive: in place, follows the actor (no teleport/pin)
		for (size_t i = 0; i < a_files.size(); i++) {
			ScenePlan::Stage stage;
			stage.files = { a_files[i] };
			stage.loops = a_loops[i];    // loop-count advance; <= 0 = hold this phase
			stage.timer = 0.0f;
			stage.blendIn = a_blends[i];
			plan.stages.push_back(std::move(stage));
		}
		const std::vector<RE::Actor*> actors{ a_actor };
		return PlaySceneStaged(actors, plan, 0);
	}

	void GraphManager::StopSceneLocked(Scene* a_scene)
	{
		// Detach every participant of this scene and drop their graphs; the engine's own rig refresh restores the game pose next frame.
		auto sceneIter = std::find_if(scenes.begin(), scenes.end(),
			[&](const std::shared_ptr<Scene>& s) { return s.get() == a_scene; });
		if (sceneIter == scenes.end()) {
			return;
		}

		const bool anchored = (*sceneIter)->anchored;

		auto& participants = (*sceneIter)->participants;
		for (auto& p : participants) {
			{
				// graph stays in the map and fades to the engine pose; the update hook queues its removal once the ramp elapses
				std::scoped_lock gl{ p->stateLock };
				p->DetachAndFadeOut();
			}
			// Revert the animation-driven movement switch from PlaySceneStaged (anchored scenes only). Game-thread only.
			if (anchored && p->target) {
				// participants are always Actors (PlayScene takes Actor*)
				RE::NiPointer<RE::Actor> keepAlive{ static_cast<RE::Actor*>(p->target.get()) };
				SFSE::GetTaskInterface()->AddTask([keepAlive]() {
					if (auto* ctl = keepAlive->movementController) {
						ctl->SetMotionDriven();
						REX::TRACE("[Anim] Actor {:X}: movement reverted motion-driven", keepAlive->formID);
					}
				});
			}
		}
		scenes.erase(sceneIter);

		REX::INFO("[Anim] Stopped scene");
	}

	void GraphManager::QueueAutoEndIfFinished(Graph& a_graph)
	{
		// The last timed/loop-counted stage ran out. Defer the stop to the game thread (StopScene needs
		// stateLock unique; the hook holds it shared). The endQueued load here is a cheap early-out; the
		// authoritative once-only gate is the exchange inside QueueSceneEndDeferred. Capturing the
		// shared_ptr keeps the Scene alive + ABA-safe.
		if (!a_graph.scene || !a_graph.scene->ended.load(std::memory_order_relaxed) ||
			a_graph.scene->endQueued.load(std::memory_order_relaxed)) {
			return;
		}
		std::shared_ptr<Scene> keepAlive;
		for (const auto& s : scenes) {
			if (s.get() == a_graph.scene) {
				keepAlive = s;
				break;
			}
		}
		if (!keepAlive) {
			REX::ERROR("[Anim] Scene end: scene not found in the live list — cannot stop (this should be impossible)");
			return;
		}
		REX::DEBUG("[Anim] Scene finished its last stage — queueing stop task (from job thread)");
		QueueSceneEndDeferred(std::move(keepAlive));
	}

	void GraphManager::QueueSceneEndDeferred(std::shared_ptr<Scene> a_scene)
	{
		// Fire once: a concurrent caller (auto-end vs stall watchdog) that loses the exchange no-ops.
		if (!a_scene || a_scene->endQueued.exchange(true, std::memory_order_relaxed)) {
			return;
		}
		SFSE::GetTaskInterface()->AddTask([keepAlive = std::move(a_scene)]() {
			// SetStage between the queue and now revives the scene (clears `ended`)
			// a revived scene must not be killed by this stale task.
			if (!keepAlive->ended.load(std::memory_order_relaxed)) {
				REX::DEBUG("[Anim] Scene end task: scene was revived meanwhile — not stopping");
				return;
			}
			REX::DEBUG("[Anim] Scene end task executing on game thread");
			auto& gm = GetSingleton();

			// The scene runtime gets first refusal: a graph-driven scene takes the matching auto-edge (advance to the next node / end its scene) and owns the teardown.
			// A standalone scene (a direct StartScene* with no graph) isn't claimed, we stop it ourselves.
			// No manager lock is held here, so the handler is free to call PlaySceneStaged/StopScene.
			bool handled = false;
			if (gm._autoEndHandler) {
				std::vector<RE::Actor*> actors;
				for (const auto& p : keepAlive->participants) {
					if (p && p->target) {
						// scene participants are always actors (PlayScene takes Actor*)
						actors.push_back(static_cast<RE::Actor*>(p->target.get()));
					}
				}
				handled = gm._autoEndHandler(actors, keepAlive->endReason.load(std::memory_order_relaxed));
			}
			if (!handled) {
				gm.StopSceneByPtr(keepAlive.get());
			}
		});
	}

	void GraphManager::StallWatchTick()
	{
		// Thresholds (steady-clock ms). Set well above a frame so a playable-FPS hitch can't trip it; the
		// scene must look dead for kSceneStallMs of CONTINUOUS engine-running time after any resume grace.
		constexpr std::int64_t kStallHookResumeGapMs = 500;   // gap since our last call that means a global pause/load
		constexpr std::int64_t kStallGraceMs         = 2000;  // after a resume, don't judge scenes (let them re-stamp)
		constexpr std::int64_t kSceneStallMs         = 1500;  // a live scene unticked this long (game running) = interrupted
		constexpr std::int64_t kStallScanIntervalMs  = 250;   // throttle the scene scan (this runs ~7x/frame)

		using namespace std::chrono;
		const std::int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();

		// Pause/resume filter. This hook stops being called when the game pauses (focus loss, menus,
		// loading). On resume every scene's lastAdvanceMs looks ancient though nothing died — so when the
		// gap since our previous call is large, (re)arm a grace window and let live scenes re-stamp before
		// we judge. The first-ever call (prev == 0) arms it too.
		const std::int64_t prev = _stallLastHookMs.exchange(now, std::memory_order_relaxed);
		if (prev == 0 || now - prev > kStallHookResumeGapMs) {
			_stallArmedMs.store(now + kStallGraceMs, std::memory_order_relaxed);
			return;
		}
		if (now < _stallArmedMs.load(std::memory_order_relaxed)) {
			return;  // inside the post-resume grace window
		}

		// Throttle the scan (a few checks/sec suffice; the bookkeeping above already ran this call).
		if (now - _stallLastScanMs.load(std::memory_order_relaxed) < kStallScanIntervalMs) {
			return;
		}
		_stallLastScanMs.store(now, std::memory_order_relaxed);

		// Collect stalled scenes under the shared lock; act on them OUTSIDE it.
		std::vector<std::shared_ptr<Scene>> stalled;
		{
			std::shared_lock l{ stateLock };
			if (scenes.empty()) {
				return;
			}
			for (const auto& s : scenes) {
				if (!s || s->ended.load(std::memory_order_relaxed) || s->endQueued.load(std::memory_order_relaxed)) {
					continue;  // already terminal / queued — leave it to the normal path
				}
				const std::int64_t lastAdv = s->lastAdvanceMs.load(std::memory_order_relaxed);
				if (lastAdv != 0 && now - lastAdv > kSceneStallMs) {
					stalled.push_back(s);
				}
			}
		}
		for (auto& s : stalled) {
			REX::WARN("[Anim] Scene stalled {}ms (engine stopped ticking it — actor unloaded / AI-disabled / interrupted) — ending it as interrupted",
				now - s->lastAdvanceMs.load(std::memory_order_relaxed));
			s->endReason.store(SceneEndReason::kInterrupted, std::memory_order_relaxed);
			s->ended.store(true, std::memory_order_relaxed);  // hand to the normal deferred-end machinery
			QueueSceneEndDeferred(s);
		}
	}

	void GraphManager::QueueTimedMarksIfFired(Graph& a_graph)
	{
		if (!a_graph.scene || !_timedMarkHandler) {
			return;
		}
		// Drain the marks the scene fired this frame (token-gated, so only the advancing graph populates them, any participant draining gets them once).
		std::vector<FiredMark> marks;
		a_graph.scene->DrainFiredMarks(marks);
		if (marks.empty()) {
			return;
		}
		// Snapshot the participant actors (NiPointer keeps them alive across the deferred task); 
		// dispatch on the game thread, the handler enters the VM (and may transition).
		std::vector<RE::NiPointer<RE::Actor>> keep;
		for (auto& p : a_graph.scene->participants) {
			if (p && p->target) {
				keep.emplace_back(static_cast<RE::Actor*>(p->target.get()));
			}
		}
		SFSE::GetTaskInterface()->AddTask([keep, marks]() {
			std::vector<RE::Actor*> actors;
			actors.reserve(keep.size());
			for (auto& a : keep) {
				actors.push_back(a.get());
			}
			if (GetSingleton()._timedMarkHandler) {
				GetSingleton()._timedMarkHandler(actors, marks);
			}
		});
	}

	void GraphManager::QueueFadeRemovalIfDone(Graph& a_graph)
	{
		// Fade-out finished: queue removal on the game thread (the hook holds the state lock shared here). 
		// A replay before the task runs resets the blend state, and RemoveFadedGraph re-checks under the lock.
		if (a_graph.scene || !a_graph.IsFadedOut() || a_graph.removalQueued) {
			return;
		}
		a_graph.removalQueued = true;
		RE::NiPointer<RE::TESObjectREFR> keepAlive{ a_graph.target };
		SFSE::GetTaskInterface()->AddTask([keepAlive]() {
			GetSingleton().RemoveFadedGraph(keepAlive.get());
		});
	}

	void GraphManager::LogSceneDiag(Graph& a_graph, RE::TESObjectREFR* a_refr)
	{
		// Update calls arrive ~7x per render frame (subdivided dt), so cadences are in update-calls, not frames: ~240 calls ≈ 0.5s.
		// NOTE: the root-node world transform is the physics capsule position, NOT the rendered skeleton
		// the compose-root pin redirects the rig compose input, and mapped bones get their worlds from the rig buffers.
		// ~0.3m capsule offset  per co-located actor is expected (and lines up correctly on screen); a jump beyond ~0.5m means physics dragged the actor.
		if (!a_graph.scene || a_graph.participantIndex < 0) {
			return;
		}
		const auto frames = ++a_graph.sceneFrames;
		if (frames != 480 && frames % 4800 != 0) {
			return;
		}
		float capsuleErr = 0.0f;
		if (a_graph.lastRoot) {
			const auto& pl = a_graph.scene->placements[a_graph.participantIndex];
			const RE::NiPoint3 world = PlacementToWorld(a_graph.scene->anchorPos, a_graph.scene->anchorHeading, pl);
			const auto& rootPos = a_graph.lastRoot->world.translate;
			const float ex = rootPos.x - world.x, ey = rootPos.y - world.y, ez = rootPos.z - world.z;
			capsuleErr = std::sqrt(ex * ex + ey * ey + ez * ez);
		}
	}

	void GraphManager::Hook_AnimGraphUpdate(void* a_this, RE::BSAnimationUpdateData* a_updateData)
	{
		// Evaluate the game's graph first; our rig writes land after the engine's pose refresh + eval and before world composition.
		_origAnimGraphUpdate(a_this, a_updateData);

		if (!a_updateData) {
			return;
		}

		auto& gm = GetSingleton();

		// Player camera guard: POVSwitch stays enabled for scroll-zoom, so first person must be bounced if the player zooms all the way in while a standalone camera lock is held. 
		// Above the managed-graph filter — its own atomic early-out makes the idle case free, and a standalone lock with no live graph still bounces.
		Camera::CameraService::GetSingleton().Tick();
		UI::FadeService::GetSingleton().Tick();  // posts the deferred fade-in once a hold deadline passes
		UI::Subtitle::Tick();  // hides the subtitle box once a shown line's hold deadline passes
		Audio::SoundService::GetSingleton().Tick();  // moves the listener to the player + reaps finished sounds
		gm.StallWatchTick();  // end scenes the engine quietly stopped ticking (so the player lock never leaks)

		// Idle early-out: this hook fires ~7x per render frame for every AnimationManager in the game; 
		// with no managed graphs there is nothing else to do.
		if (gm.graphCount.load(std::memory_order_relaxed) == 0) {
			return;
		}

		// Identity match: resolve the managed actor directly from the AnimationManager pointer (no position guessing, no collisions).
		auto* animMgr = static_cast<RE::AnimationManager*>(a_this);
		RE::TESObjectREFR* refr = animMgr ? animMgr->GetTargetReference() : nullptr;
		if (!refr) {
			return;
		}

		std::shared_lock l{ gm.stateLock };
		auto iter = gm.graphs.find(refr);
		if (iter == gm.graphs.end()) {
			return;
		}
		// stateLock is held shared through the rest of this function, so the map entry can't be erased from under us 
		// bind to the slot by reference instead of copying the shared_ptr (drops a refcount RMW per call, and this fires ~7x/frame per managed actor).
		auto& g = iter->second;

		std::unique_lock gl{ g->stateLock };
		g->Sample(PlaybackDelta(*a_updateData), a_this);

		// Per-graph follow-ups, run under both locks (each defers any game-thread-only work to the task queue).
		gm.QueueAutoEndIfFinished(*g);
		gm.QueueTimedMarksIfFired(*g);
		gm.QueueFadeRemovalIfDone(*g);
		gm.LogSceneDiag(*g, refr);
	}

	uint64_t GraphManager::Hook_ModelNodeUpdate(RE::BGSModelNode* a_this, void* a_parentTransform, void* a_updateData)
	{
		// Stamp the latest sampled pose for the graph driving this skeleton before the engine's compose+commit runs (the verified write point).
		// Unmanaged skeletons fall through with one map scan; managed graph counts are small (scene participants).
		auto& gm = GetSingleton();
		// This runs once per skeleton per frame game-wide; with no OSF playback the atomic check keeps it lock-free. 
		// A racing insert is benign (the new graph stamps next frame at the latest).
		if (gm.graphCount.load(std::memory_order_relaxed) > 0) {
			std::shared_lock l{ gm.stateLock };
			if (!gm.graphs.empty()) {
				for (auto& [refr, g] : gm.graphs) {
					// benign unsynchronized pointer read; verified again under the graph lock inside StampPose
					if (g->StampTarget() == a_this) {
						std::unique_lock gl{ g->stateLock };
						g->StampPose(a_this);
						// Pin the compose root TRANSLATION to the placement world position. a_parentTransform = &fadeNode->local (this compose's root input;
						// NiTransform, translate at +0x30). Overriding the compose input pins the RENDERED skeleton without fighting physics 
						// (capsules sit ~0.3 m off and win any refr-teleport). Scene participant -> its placement (anchored); solo graph -> its SetAnchor anchor when rootMode != kFollow.
						RE::NiPoint3 pinWorld{};
						bool doPin = false;
						// World heading to re-pin per frame (radians). Only known on the scene path; solo anchors retain no heading. Gated to the PLAYER below.
						float pinHeading = 0.0f;
						bool hasPinHeading = false;
						if (g->scene && g->scene->anchored && g->participantIndex >= 0) {
							pinWorld = PlacementToWorld(g->scene->anchorPos, g->scene->anchorHeading,
								g->scene->placements[g->participantIndex]);
							pinHeading = g->scene->anchorHeading +
								g->scene->placements[g->participantIndex].heading;
							hasPinHeading = true;
							doPin = true;
						} else if (!g->scene && g->hasAnchor && g->rootMode != RootMode::kFollow) {
							pinWorld = g->anchorPos;  // additive currently pins like kPin (root-motion travel not done yet)
							doPin = true;
						}
						if (doPin && a_parentTransform) {
							float* root = reinterpret_cast<float*>(a_parentTransform);
							root[12] = pinWorld.x;  // NiTransform translation (+0x30)
							root[13] = pinWorld.y;
							root[14] = pinWorld.z;

								// Co-locate the actor's tracked position with the pinned skeleton. The engine's
								// third-person camera, the audio listener, and the scene-orbit camera all read
								// data.location, which otherwise drifts with leaked root motion (the "capsule err")
								// and frames empty space. We correct only the bookkeeping position the cameras
								// follow; the havok capsule is left alone (it wins teleports) and restores on end.
								refr->data.location = pinWorld;

							// Pin compose-root ROTATION for the PLAYER only: in 3rd person the engine rewrites the player heading from camera yaw each frame
							// (AI-driven doesn't suppress it on 1.16.244), so the rig spins as the camera orbits. 
							// NPCs stay anim-owned. Rotation = NiMatrix3 at +0x00: three ROWS of 4 floats (4th pad), stride 4 -> root[r*4 + c].
							// Write a Z-up yaw; leave the pad lanes (root[3],[7],[11]) alone.
							if (hasPinHeading &&
								refr == static_cast<RE::TESObjectREFR*>(RE::PlayerCharacter::GetSingleton())) {
								const float c = std::cos(pinHeading);
								const float s = std::sin(pinHeading);
								root[0] = c;    root[1] = -s;   root[2] = 0.0f;   // row0
								root[4] = s;    root[5] = c;    root[6] = 0.0f;   // row1
								root[8] = 0.0f; root[9] = 0.0f; root[10] = 1.0f;  // row2
							}

							// Keep the CULL SPHERE on the pinned render position. The engine derives worldBound from the physics capsule (~0.3 m off), 
							// so left alone NiCullingProcess pops the actor in/out as the camera orbits.
							// Recover the node from a_parentTransform (= local @ +0x40) and rewrite worldBound (center +0x100, radius +0x10C). 
							// Written pre-orig but after the bound pass, so culling sees it this frame.
							auto* fadeNode = reinterpret_cast<RE::NiAVObject*>(
								reinterpret_cast<std::byte*>(a_parentTransform) - offsetof(RE::NiAVObject, local));

							// Center on the visible mesh (torso), not the feet/origin: a feet-centered sphere
							// sits below the posed body and frustum-culls at certain orbit angles (see kPinCullCenterUp).
							// CAP the radius small (min, not max) so the near-fade threshold stays close in.
							fadeNode->worldBound.center = { pinWorld.x, pinWorld.y, pinWorld.z + kPinCullCenterUp };
							fadeNode->worldBound.radius = std::min(fadeNode->worldBound.radius, kPinCullRadius);

							// Hold the near-camera fade (BSFadeNode+0x1B4) at 1.0 so pinned participants don't fade
							// out when the camera orbits close (works together with the small bound radius above).
							*reinterpret_cast<float*>(
								reinterpret_cast<std::byte*>(fadeNode) + kFadeNodeVisFlagOff) = 1.0f;
						}
						break;
					}
				}
			}
		}

		return _origModelNodeUpdate(a_this, a_parentTransform, a_updateData);
	}
}
