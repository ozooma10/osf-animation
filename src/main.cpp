#include "API/Health.h"
#include "API/MinimumVersion.h"
#include "API/SceneAPIControl.h"
#include "API/UISettings.h"
#include "Animation/GraphManager.h"
#include "Camera/CameraService.h"
#include "Equipment/GearRegistry.h"
#include "Input/InputService.h"
#include "Maintenance/FrameMaintenance.h"
#include "Papyrus/OSFScript.h"
#include "Registry/RequirementRegistry.h"
#include "Registry/SceneRegistry.h"
#include "Registry/SoundRegistry.h"
#include "Scene/SceneRuntime.h"
#include "Serialization/PersistenceHost.h"
#include "Serialization/SaveSafety.h"
#include "Util/CrashHandler.h"
#include <REX/W32/KERNEL32.h>
#include <nlohmann/json.hpp>

namespace
{
	// Last game version whose raw offsets were hand-checked.
	constexpr REL::Version kVerifiedGameVersion{ 1, 16, 244, 0 };
	// Persistence gateways/branches use 101 bytes; the main-loop call hook adds up to 14.
	constexpr std::size_t kTrampolineBytes = 192;

	void MessageCallback(SFSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SFSE::MessagingInterface::kPostDataLoad: {
			// Before the loaders, which are the first health producers; also flushes load-time reports.
			OSF::API::Health::Connect();
			OSF::Registry::RequirementRegistry::LoadAll();
			OSF::API::MinimumVersion::EnablePrompts();
			OSF::Registry::ContentRegistry::GetSingleton().LoadAll();
			OSF::Registry::SoundRegistry::GetSingleton().LoadAll();
			OSF::API::Health::ReportRegistryLoad();
			OSF::Equipment::Gear::LoadAll();
			OSF::Scene::SceneRuntime::GetSingleton().RegisterWithGraphManager();
			if (!OSF::Papyrus::RegisterFunctions()) {
				REX::ERROR("[Boot] GameVM not available at kPostDataLoad, papyrus natives not registered");
			}
			OSF::API::MarkReady();
			// After the registries so initial settings apply to loaded services.
			OSF::API::InstallUISettings();
			OSF::Serialization::SaveSafety::RegisterLoadEventSinks();

			REX::INFO("[Feature] Main Animation Playback Hooks {}", OSF::Animation::GraphManager::GetSingleton().HooksInstalled() ? "INSTALLED" : "UNAVAILABLE");
			REX::INFO("[Feature] Main-Loop Frame Hook INSTALLED");
			REX::INFO("[Feature] Raw-layout Camera Controls {}",
				OSF::Camera::CameraService::GetSingleton().RawLayoutSupport() ? "AVAILABLE" : "UNAVAILABLE");
			REX::INFO("[Feature] Scene Props AVAILABLE");
			break;
		}
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
	OSF::Util::CrashHandler::Install();

	SFSE::Init(a_sfse, { .trampoline = true, .trampolineSize = kTrampolineBytes });

	return true;
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse, { .trampoline = true, .trampolineSize = kTrampolineBytes });

	const auto runtime = a_sfse->RuntimeVersion();
	OSF::Camera::CameraService::GetSingleton().SetRawLayoutSupport(runtime == kVerifiedGameVersion);
	REX::INFO("[Boot] {} v{} loading — supported game version {}, running on {}",
		SFSE::GetPluginName(), SFSE::GetPluginVersion().string(),
		kVerifiedGameVersion.string(), runtime.string());
	if (runtime != kVerifiedGameVersion) {
		REX::WARN("[Boot] Unsupported game version: OSF Animation was last tested against Starfield version {} only, but this is {}. "
			"Not all features may be available",
			kVerifiedGameVersion.string(), runtime.string(), kVerifiedGameVersion.string());
		// Buffered until Health::Connect at kPostDataLoad.
		const nlohmann::json context{
			{ "tested", kVerifiedGameVersion.string() },
			{ "running", runtime.string() },
		};
		OSF::API::Health::Report("boot.game-version", "boot.untested-game-version",
			OSF::API::Health::Severity::kWarning, runtime.string(), &context);
	}

	if (!OSF::Maintenance::InstallFrameMaintenance()) {
		return false;
	}
	OSF::Animation::GraphManager::GetSingleton().InstallHooks();
	OSF::Serialization::PersistenceHost::Initialize();
	SFSE::GetMessagingInterface()->RegisterListener(MessageCallback);

	return true;
}
