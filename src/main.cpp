#include "Animation/AfxStubber.h"
#include "Animation/GraphManager.h"
#include "Animation/IdlePlayer.h"
#include "Audio/SoundService.h"
#include "Config/Settings.h"
#include "Papyrus/OSFScript.h"
#include "Registry/PackRegistry.h"
#include "Registry/SceneRegistry.h"
#include "Scene/SceneRuntime.h"
#include "Serialization/SaveSafety.h"
#include "UI/CompatWarning.h"

#include <REX/W32/KERNEL32.h>

namespace
{
	// Last game version whose offsets I checked by hand. Should keep working unless AddressLib ships a breaking update.
	constexpr REL::Version kVerifiedGameVersion{ 1, 16, 244, 0 };

	void MessageCallback(SFSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SFSE::MessagingInterface::kPostLoad:
			// Every SFSE plugin's load handler has run by now, so an incompatible framework's DLL (if any) is in-process for the module probe.
			OSF::UI::CompatWarning::ProbeIncompatibilities();
			break;
		case SFSE::MessagingInterface::kPostDataLoad:
			OSF::Registry::PackRegistry::GetSingleton().LoadAll();
			OSF::Registry::SceneRegistry::GetSingleton().LoadAll();
			OSF::Scene::SceneRuntime::GetSingleton().RegisterWithGraphManager();
			// Spin up the audio device now — it is slow — so the first sound cue doesn't stall a job thread.
			OSF::Audio::SoundService::GetSingleton().Init();
			// Apply the user's safety toggles now that the services they configure exist.
			OSF::Config::Settings::Load();
			// Resolve the fixed idle-playback template/keyword refs
			OSF::Animation::IdlePlayer::GetSingleton().Resolve();
			if (!OSF::Papyrus::RegisterFunctions()) {
				REX::ERROR("GameVM not available at kPostDataLoad, papyrus natives not registered");
			}
			OSF::Serialization::SaveSafety::RegisterLoadEventSinks();

			REX::INFO("FEATURE: Main Animation Playback Hooks {}", OSF::Animation::GraphManager::GetSingleton().HooksInstalled() ? "INSTALLED" : "UNAVAILABLE");
			break;
		default:
			break;
		}
	}
}

SFSE_PLUGIN_PRELOAD(const SFSE::PreLoadInterface* a_sfse)
{
	SFSE::Init(a_sfse);

	return true;
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse);

	const auto runtime = a_sfse->RuntimeVersion();
	REX::INFO("{} v{} loading — supported game version {}, running on {}",
		SFSE::GetPluginName(), SFSE::GetPluginVersion().string(),
		kVerifiedGameVersion.string(), runtime.string());
	if (runtime != kVerifiedGameVersion) {
		REX::WARN("Unsupported game version: OSF Animation was last tested against Starfield version {} only, but this is {}. "
			"Not all features may be available",
			kVerifiedGameVersion.string(), runtime.string(), kVerifiedGameVersion.string());
	}

	// Write stub .afx sidecars for any loose custom .af missing one, so authors ship only `.af`.
	// Must run here (load time) — the engine reads .afx during its startup AnimTextData scan, which
	// is after this but before gameplay; doing it at kPostDataLoad would be a launch too late.
	OSF::Animation::EnsureAfxStubs();

	OSF::Animation::GraphManager::GetSingleton().InstallHooks();
	SFSE::GetMessagingInterface()->RegisterListener(MessageCallback);

	REX::INFO("Load complete");

	return true;
}
