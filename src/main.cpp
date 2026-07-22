#include "API/SceneAPIControl.h"
#include "API/UIBridge.h"
#include "API/UISettings.h"
#include "Animation/GraphManager.h"
#include "Equipment/GearRegistry.h"
#include "Input/InputService.h"
#include "Papyrus/OSFScript.h"
#include "Registry/SceneRegistry.h"
#include "Registry/SoundRegistry.h"
#include "Scene/SceneRuntime.h"
#include "Serialization/ClipDurations.h"
#include "Serialization/PersistenceHost.h"
#include "Serialization/SaveSafety.h"
#include "Util/CrashHandler.h"

#include <REX/W32/KERNEL32.h>

namespace
{
	// Last game version whose offsets I checked by hand. Should keep working unless AddressLib ships a breaking update.
	constexpr REL::Version kVerifiedGameVersion{ 1, 16, 244, 0 };
	// Persistence uses 59 bytes for three original-function gateways plus 42 bytes
	// for their branch islands. Keep a small margin for hook implementation changes.
	constexpr std::size_t kTrampolineBytes = 128;

	void MessageCallback(SFSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SFSE::MessagingInterface::kPostDataLoad:
			OSF::Registry::SceneRegistry::GetSingleton().LoadAll();
			OSF::Registry::SoundRegistry::GetSingleton().LoadAll();
			OSF::Equipment::Gear::LoadAll();
			OSF::Scene::SceneRuntime::GetSingleton().RegisterWithGraphManager();
			if (!OSF::Papyrus::RegisterFunctions()) {
				REX::ERROR("[Boot] GameVM not available at kPostDataLoad, papyrus natives not registered");
			}
			OSF::API::MarkReady();
			// Register the osf.* animation-browser commands on OSF UI's native bridge (no-op if OSF UI absent).
			// Runs after the registry is loaded and the scene API is ready, so catalog/launch answer live.
			OSF::API::InstallUIBridge();
			// Settings + hotkeys live in OSF UI's MCM (schema registration, change
			// subscription, hotkey dispatch); degrades loudly to defaults when OSF UI
			// is absent/too old. After InstallUIBridge: same bridge singleton.
			OSF::API::InstallUISettings();
			// Probe clip loop lengths for the catalog's time estimates After InstallUIBridge so the push hook exists.
			OSF::Serialization::ClipDurations::ScanSceneClipsAsync(&OSF::API::PushCatalogUpdate);
			OSF::Serialization::SaveSafety::RegisterLoadEventSinks();

			REX::INFO("[Feature] Main Animation Playback Hooks {}", OSF::Animation::GraphManager::GetSingleton().HooksInstalled() ? "INSTALLED" : "UNAVAILABLE");
			break;
		case SFSE::MessagingInterface::kPostPostDataLoad:
			REX::INFO("[Feature] Input Hook {}", OSF::Input::InputService::GetSingleton().Install() ? "INSTALLED" : "UNAVAILABLE");
			OSF::Animation::GraphManager::GetSingleton().RegisterConsolePauseSink();
			break;
		default:
			break;
		}
	}
}

SFSE_PLUGIN_PRELOAD(const SFSE::PreLoadInterface* a_sfse)
{
	// Earliest possible — before anything (ours or commonlib's) can assert/crash. Captures
	// CRT asserts (which trainwreck cannot see) + a stack + minidump into the SFSE Logs folder.
	OSF::Util::CrashHandler::Install();

	SFSE::Init(a_sfse, { .trampoline = true, .trampolineSize = kTrampolineBytes });

	return true;
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse, { .trampoline = true, .trampolineSize = kTrampolineBytes });

	const auto runtime = a_sfse->RuntimeVersion();
	REX::INFO("[Boot] {} v{} loading — supported game version {}, running on {}",
		SFSE::GetPluginName(), SFSE::GetPluginVersion().string(),
		kVerifiedGameVersion.string(), runtime.string());
	if (runtime != kVerifiedGameVersion) {
		REX::WARN("[Boot] Unsupported game version: OSF Animation was last tested against Starfield version {} only, but this is {}. "
			"Not all features may be available",
			kVerifiedGameVersion.string(), runtime.string(), kVerifiedGameVersion.string());
	}

	OSF::Animation::GraphManager::GetSingleton().InstallHooks();
	OSF::Serialization::PersistenceHost::Initialize();
	SFSE::GetMessagingInterface()->RegisterListener(MessageCallback);

	return true;
}
