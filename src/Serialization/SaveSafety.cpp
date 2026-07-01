#include "Serialization/SaveSafety.h"

#include "Animation/GraphManager.h"
#include "Papyrus/OSFScript.h"
#include "Scene/SceneEventRelay.h"
#include "Util/Hooking.h"

#include <array>

namespace OSF::Serialization::SaveSafety
{
	namespace
	{
		bool SaveLoadEventSourcePrologueMatches()
		{
			// 1.16.242/.244 magic-static GetEventSource prologue for
			// BSTGlobalEvent::EventSource<SaveLoadEvent>. If this drifts, avoid
			// registering a sink through a stale AddressLib binding.
			constexpr std::array<std::uint8_t, 13> kExpectedPrologue{
				0x48, 0x83, 0xEC, 0x28, 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00
			};

			return Util::Hooking::PrologueMatches(RE::ID::SaveLoadEvent::GetEventSource, kExpectedPrologue);
		}

		bool IsWorldReplacingLoadOp(RE::SaveLoadEvent::OpType a_opType)
		{
			using OpType = RE::SaveLoadEvent::OpType;
			return a_opType == OpType::kLoadMostRecent ||
			       a_opType == OpType::kQuickload ||
			       a_opType == OpType::kLoad ||
			       a_opType == OpType::kLoadNamedFile;
		}

		// Ops that reset the Papyrus VM (and so free every live script Object): the world-replacing
		// loads PLUS quit-to-menu / quit-to-desktop. Native code caching script-Object references must
		// drop them at kBegin (VM still alive) before they dangle. Quit-to-menu also matters because it
		// is the gateway to a New Game, which fires no SaveLoadEvent of its own.
		bool IsVMEndingOp(RE::SaveLoadEvent::OpType a_opType)
		{
			using OpType = RE::SaveLoadEvent::OpType;
			return IsWorldReplacingLoadOp(a_opType) ||
			       a_opType == OpType::kExitSaveToMainMenu ||
			       a_opType == OpType::kExitSaveToDesktop;
		}

		// Ops that WRITE a save. kBegin fires ~2 frames before the save executes (autosave) or from the
		// pause menu (manual/quicksave); the exit ops write an exit save before leaving the world.
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
					// Save window: strip the (plausibly persistent — the sibling player AI-driven flag is
					// PROVEN persistent) kAnimationDriven bit from anchored NPC participants before the save
					// serializes actor state, so loading a mid-scene save can't reload an NPC permanently
					// animation-driven. Re-asserted on the terminal status below. Runs BEFORE the relay/
					// StopAll handling so the exit-save ops write the scrubbed state too.
					if (IsSaveOp(a_event.opType)) {
						Animation::GraphManager::GetSingleton().OnSaveBegin();
					}
					// Drop the relay's cached Papyrus Object receivers while the VM is still alive. kBegin
					// fires on the main thread strictly before the VM teardown, so this is the only safe
					// window: afterwards those BSTSmartPointer<Object> receivers dangle and crash when the
					// slot is next dropped (UnregisterSceneCallback) or dispatched. Covers loads AND
					// quit-to-menu/desktop — both reset the VM.
					if (IsVMEndingOp(a_event.opType)) {
						Scene::SceneEventRelay::GetSingleton().Clear();
					}
					// StopAll stays scoped to world-replacing loads: it releases the player-control lock /
					// AI-driven flag, which must not be perturbed mid-save on a quit op.
					if (IsWorldReplacingLoadOp(a_event.opType)) {
						Animation::GraphManager::GetSingleton().StopAll("save-load begin");
					}
				} else {
					// Any terminal status (kSaveCompleted, or kFailed = failure/cancel of any op) closes the
					// save window and re-asserts the hold. A cheap no-op when no window is open — load
					// statuses land here too.
					Animation::GraphManager::GetSingleton().OnSaveEnd();
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
				Animation::GraphManager::GetSingleton().StopAll("save loaded (TESLoadGameEvent backstop)");
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void RegisterLoadEventSinks()
	{
		if (!SaveLoadEventSourcePrologueMatches()) {
			REX::WARN("[Save] SaveLoadEvent GetEventSource prologue mismatch; relying on TESLoadGameEvent backstop");
		} else if (auto* saveLoadSrc = RE::SaveLoadEvent::GetEventSource(); saveLoadSrc) {
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
