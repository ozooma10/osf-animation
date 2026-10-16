#include "Serialization/SaveSafety.h"

#include "Animation/GraphManager.h"
#include "Camera/CameraService.h"
#include "Papyrus/OSFScript.h"
#include "Player/PlayerInputLockService.h"
#include "Scene/SceneEventRelay.h"
#include "Scene/SceneRuntime.h"
#include "Serialization/PersistenceHost.h"

namespace OSF::Serialization::SaveSafety
{
	namespace
	{
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
						Animation::GraphManager::GetSingleton().OnSaveBegin();
						Scene::SceneRuntime::GetSingleton().OnSaveBegin();
						Player::PlayerInputLockService::GetSingleton().OnSaveBegin();
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
					Scene::SceneRuntime::GetSingleton().OnSaveEnd();
					Player::PlayerInputLockService::GetSingleton().OnSaveEnd();
					Animation::GraphManager::GetSingleton().OnSaveEnd();
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
