#include "Serialization/SaveSafety.h"
#include "Profiling/Profile.h"

#include "Animation/GraphManager.h"
#include "Camera/CameraService.h"
#include "Papyrus/OSFScript.h"
#include "Player/PlayerInputLockService.h"
#include "Scene/SceneEventRelay.h"
#include "Scene/SceneRuntime.h"
#include "Serialization/PersistenceHost.h"
#include "Util/Hooking.h"
#include "REL/THook.h"

#include <optional>

namespace OSF::Serialization::SaveSafety
{
	namespace
	{
		using RequestedSave = bool(RE::BGSSaveLoadManager*, const char*, std::int32_t, std::uint32_t);
		std::optional<REL::THook<RequestedSave>> g_requestedSaveHook;

		void BeginSave()
		{
			OSF_PROFILE_SCOPE("Save.Begin");
			Animation::GraphManager::GetSingleton().OnSaveBegin();
			Scene::SceneRuntime::GetSingleton().OnSaveBegin();
			Player::PlayerInputLockService::GetSingleton().OnSaveBegin();
		}

		void EndSave()
		{
			OSF_PROFILE_SCOPE("Save.End");
			Scene::SceneRuntime::GetSingleton().OnSaveEnd();
			Player::PlayerInputLockService::GetSingleton().OnSaveEnd();
			Animation::GraphManager::GetSingleton().OnSaveEnd();
		}

		bool RequestedSaveThunk(RE::BGSSaveLoadManager* manager, const char* name,
			std::int32_t slot, std::uint32_t flags)
		{
			// Game.RequestSave's bit-1 path omits SaveLoadEvent Begin. This call is
			// synchronous on the main request pump, before serialization starts.
			// Do not move inventory mutations into the persistence writer's worker hook.
			BeginSave();
			struct ResumeOnReturn { ~ResumeOnReturn() { EndSave(); } } resume;
			return (*g_requestedSaveHook)(manager, name, slot, flags);
		}

		bool InstallRequestedSaveHook()
		{
			// 1.16.244: 98691+0x5E2 -> 98622. Gate the argument setup and destination;
			// the following call reloads its arguments and consumes no volatile result.
			// Canonical evidence: OSF RE engine.save_load, Haters save-timeout run.
			constexpr REL::ID pump{ 98691 }, save{ 98622 };
			constexpr std::ptrdiff_t offset = 0x5E2;
			constexpr std::array<std::uint8_t, 13> arguments{
				0x45, 0x33, 0xC9, 0x45, 0x8D, 0x41, 0xFF,
				0x48, 0x8B, 0xD0, 0x49, 0x8B, 0xCE
			};
			const auto site = pump.address() + offset;
			if (!Util::Hooking::PrologueMatches(site - arguments.size(), arguments) ||
				*reinterpret_cast<const std::uint8_t*>(site) != 0xE8 ||
				REL::ASM::CALL5::TARGET(site) != save.address()) {
				REX::ERROR("[Save] requested-save hook rejected: native dispatch differs from verified 1.16.244");
				return false;
			}
			g_requestedSaveHook.emplace("[Save] RequestSave", pump, offset, &RequestedSaveThunk);
			return g_requestedSaveHook->Init() && g_requestedSaveHook->Enable();
		}

		bool IsWorldReplacingLoadOp(RE::SaveLoadEvent::OpType a_opType)
		{
			using OpType = RE::SaveLoadEvent::OpType;
			return a_opType == OpType::kLoadMostRecent ||
			       a_opType == OpType::kQuickload ||
			       a_opType == OpType::kLoad ||
			       a_opType == OpType::kLoadNamedFile;
		}

		// Ops that reset the Papyrus VM. Quit-to-menu matters too: New Game fires no SaveLoadEvent of its own.
		bool IsVMEndingOp(RE::SaveLoadEvent::OpType a_opType)
		{
			using OpType = RE::SaveLoadEvent::OpType;
			return IsWorldReplacingLoadOp(a_opType) ||
			       a_opType == OpType::kExitSaveToMainMenu ||
			       a_opType == OpType::kExitSaveToDesktop;
		}

		bool IsSaveOp(RE::SaveLoadEvent::OpType a_opType)
		{
			using OpType = RE::SaveLoadEvent::OpType;
			return a_opType == OpType::kAutosave ||
			       a_opType == OpType::kQuicksave ||
			       a_opType == OpType::kManualSave ||
			       a_opType == OpType::kExitSaveToMainMenu ||
			       a_opType == OpType::kExitSaveToDesktop;
		}

		class SaveLoadSink : public RE::BSTEventSink<RE::SaveLoadEvent>
		{
		public:
			static SaveLoadSink* GetSingleton()
			{
				static SaveLoadSink instance;
				return &instance;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::SaveLoadEvent& a_event,
				RE::BSTEventSource<RE::SaveLoadEvent>*) override
			{
				if (a_event.status == RE::SaveLoadEvent::Status::kBegin) {
					// Strip NPC kAnimationDriven before actor state serializes so a mid-scene save can't bake it in.
					// Must run first so exit saves write the scrubbed state too.
					if (IsSaveOp(a_event.opType)) {
						BeginSave();
					}
					// Cached Papyrus receivers dangle after VM teardown; kBegin is the last safe point to drop them.
					if (IsVMEndingOp(a_event.opType)) {
						Scene::SceneEventRelay::GetSingleton().Clear();
					}
					// Loads only: saving must retain live scene ownership, including exit saves.
					if (IsWorldReplacingLoadOp(a_event.opType)) {
						PersistenceHost::BeginLoad("SaveLoadEvent begin");
					}
				} else {
					// Any terminal status closes the save window; no-op when none is open.
					EndSave();
					if (IsWorldReplacingLoadOp(a_event.opType)) {
						PersistenceHost::AbortLoad();
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class LoadGameSink : public RE::BSTEventSink<RE::TESLoadGameEvent>
		{
		public:
			static LoadGameSink* GetSingleton()
			{
				static LoadGameSink instance;
				return &instance;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESLoadGameEvent&,
				RE::BSTEventSource<RE::TESLoadGameEvent>*) override
			{
				if (!Papyrus::RegisterFunctions()) {
					REX::ERROR("[Save] could not re-register OSF natives after load (GameVM unavailable); "
						"OSF.* stays unbound until the next successful load");
				}
				PersistenceHost::OnLoadBackstop();
				// The save itself may carry the AI-driven flag (free-camera saves), leaving the player stuck.
				// No full StopAll here: it would discard scenes persistence clients just restored.
				Player::PlayerInputLockService::GetSingleton().ClearAIDriven();
				// Recovers a camera left in an OSF alt state with its driver stopped.
				Camera::CameraService::GetSingleton().OnPostLoad();
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void RegisterLoadEventSinks()
	{
		PersistenceHost::RegisterEventSinks();
		REX::INFO("[Feature] Requested-Save Safety Hook {}", InstallRequestedSaveHook() ? "INSTALLED" : "UNAVAILABLE");
		if (auto* saveLoadSrc = RE::SaveLoadEvent::GetEventSource()) {
			saveLoadSrc->RegisterSink(SaveLoadSink::GetSingleton());
			REX::DEBUG("[Save] registered SaveLoadEvent begin sink");
		} else {
			REX::WARN("[Save] SaveLoadEvent source null; relying on TESLoadGameEvent backstop");
		}

		if (auto* src = RE::TESLoadGameEvent::GetEventSource()) {
			src->RegisterSink(LoadGameSink::GetSingleton());
			REX::DEBUG("[Save] registered TESLoadGameEvent backstop sink");
		} else {
			REX::ERROR("[Save] TESLoadGameEvent source null; Something very wrong");
		}
	}
}
