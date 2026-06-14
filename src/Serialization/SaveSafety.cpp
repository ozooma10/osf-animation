#include "Serialization/SaveSafety.h"

#include "Animation/GraphManager.h"
#include "Papyrus/OSFScript.h"
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

		// Full opType enumeration statically proven 2026-06-12 (osf-re
		// engine.save_load: all 10 FireBegin sites + every request-bit setter,
		// 1.16.242 + 1.16.244). World-replacing loads are exactly these four —
		// kLoadMostRecent is the DEATH RELOAD path, which the previous
		// {4, 6}-only set missed. New game and Unity/NG+ never enter the
		// manager pump and fire no SaveLoadEvent at all (TESLoadGameEvent
		// backstop still covers them).
		bool IsWorldReplacingLoadOp(RE::SaveLoadEvent::OpType a_opType)
		{
			using OpType = RE::SaveLoadEvent::OpType;
			return a_opType == OpType::kLoadMostRecent ||
			       a_opType == OpType::kQuickload ||
			       a_opType == OpType::kLoad ||
			       a_opType == OpType::kLoadNamedFile;
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
				if (a_event.status == RE::SaveLoadEvent::Status::kBegin && IsWorldReplacingLoadOp(a_event.opType)) {
					Animation::GraphManager::GetSingleton().StopAll("save-load begin");
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
				// Starfield rebuilds the Papyrus VM on every load (the script log
				// closes and reopens), dropping the natives OSF bound once at
				// kPostDataLoad. Re-bind them onto the fresh VM here, or every OSF.*
				// call from consumers — including the SAF->OSF shim that drives
				// legacy SAF mods — fails with "Unbound native function" and all
				// playback silently no-ops. This fires post-load (VM is back up).
				if (!Papyrus::RegisterFunctions()) {
					REX::WARN("SaveSafety: could not re-register OSF natives after load (GameVM unavailable); "
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
			REX::WARN("SaveSafety: SaveLoadEvent GetEventSource prologue mismatch; relying on TESLoadGameEvent backstop");
		} else if (auto* saveLoadSrc = RE::SaveLoadEvent::GetEventSource(); saveLoadSrc) {
			saveLoadSrc->RegisterSink(SaveLoadSink::GetSingleton());
			REX::INFO("SaveSafety: registered SaveLoadEvent begin sink");
		} else {
			REX::WARN("SaveSafety: SaveLoadEvent source null; relying on TESLoadGameEvent backstop");
		}

		if (auto* src = RE::TESLoadGameEvent::GetEventSource()) {
			src->RegisterSink(LoadGameSink::GetSingleton());
			REX::INFO("SaveSafety: registered TESLoadGameEvent backstop sink");
		} else {
			REX::WARN("SaveSafety: TESLoadGameEvent source null; OSF.NotifyGameLoaded() remains the manual fallback");
		}
	}
}
