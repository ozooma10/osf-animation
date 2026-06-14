#include "Animation/GraphManager.h"
#include "Papyrus/OSFScript.h"
#include "Registry/PackRegistry.h"
#include "Serialization/SaveSafety.h"
#include "UI/CompatWarning.h"

#include <REX/W32/KERNEL32.h>

namespace
{
	//This just tracks the last game version i manually "verified" the offsets should be okay
	//(Still should be fine unless address library has breaking update)
	constexpr REL::Version kVerifiedGameVersion{ 1, 16, 244, 0 };

	// Simple report of mod status
	void LogFeatureReport()
	{
		const bool hooks = OSF::Animation::GraphManager::GetSingleton().HooksInstalled();
		REX::INFO("Feature report: playback hooks {} "
			"('unavailable' = the AnimationManager/BGSModelNode vtable verification refused this game build; "
			"see the matching error above)",
			hooks ? "INSTALLED" : "UNAVAILABLE");
	}

	void MessageCallback(SFSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SFSE::MessagingInterface::kPostLoad:
			// Every SFSE plugin's load handler has run by now, so an incompatible
			// framework's DLL (if any) is in-process for the module probe.
			OSF::UI::CompatWarning::ProbeIncompatibilities();
			break;
		case SFSE::MessagingInterface::kPostDataLoad:
			OSF::Registry::PackRegistry::GetSingleton().LoadAll();
			if (!OSF::Papyrus::RegisterFunctions()) {
				REX::ERROR("GameVM not available at kPostDataLoad, papyrus natives not registered");
			}
			OSF::Serialization::SaveSafety::RegisterLoadEventSinks();
			LogFeatureReport();
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
		REX::WARN("Unsupported game version: OSF Animation supports Starfield {} only, but this is {}. "
			"Version-locked engine bindings self-disable on a mismatch, so playback may be partly or "
			"fully unavailable — update the game to {} (or wait for a plugin update).",
			kVerifiedGameVersion.string(), runtime.string(), kVerifiedGameVersion.string());
	}

	OSF::Animation::GraphManager::GetSingleton().InstallHooks();
	SFSE::GetMessagingInterface()->RegisterListener(MessageCallback);

	REX::INFO("Load complete");

	return true;
}
