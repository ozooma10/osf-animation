#include "API/SceneAPIControl.h"
#include "Animation/GraphManager.h"
#include "Config/Settings.h"
#include "Input/InputService.h"
#include "Papyrus/OSFScript.h"
#include "Registry/SceneRegistry.h"
#include "Registry/SoundRegistry.h"
#include "Scene/SceneRuntime.h"
#include "Serialization/SaveSafety.h"
#include "Util/CrashHandler.h"

#include <REX/W32/KERNEL32.h>

namespace
{
	// Last game version whose offsets I checked by hand. Should keep working unless AddressLib ships a breaking update.
	constexpr REL::Version kVerifiedGameVersion{ 1, 16, 244, 0 };

	void MessageCallback(SFSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SFSE::MessagingInterface::kPostDataLoad:
			OSF::Registry::SceneRegistry::GetSingleton().LoadAll();
			OSF::Registry::SoundRegistry::GetSingleton().LoadAll();
			OSF::Scene::SceneRuntime::GetSingleton().RegisterWithGraphManager();
			// Apply the user's safety toggles now that the services they configure exist.
			OSF::Config::Settings::Load();
			if (!OSF::Papyrus::RegisterFunctions()) {
				REX::ERROR("[Boot] GameVM not available at kPostDataLoad, papyrus natives not registered");
			}
			OSF::API::MarkReady();
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

	SFSE::Init(a_sfse);

	return true;
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse);

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
	SFSE::GetMessagingInterface()->RegisterListener(MessageCallback);

	return true;
}
