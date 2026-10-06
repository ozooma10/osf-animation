#include "API/EngineClient.h"
#include "API/BrowserHealth.h"
#include "API/UIBridge.h"
#include "API/UISettings.h"

namespace
{
	void MessageCallback(SFSE::MessagingInterface::Message* message)
	{
		// All engines/hosts finish kPostDataLoad first; DLL enumeration order is irrelevant.
		if (message->type != SFSE::MessagingInterface::kPostPostDataLoad) return;
		OSF::API::Health::Connect();
		if (!OSF::API::EngineClient::Connect()) {
			REX::WARN("[Boot] OSF Animation engine services 1.0 unavailable; Browser is inactive");
			return;
		}
		OSF::API::InstallUIBridge();
		OSF::API::InstallUISettings();
	}
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* sfse)
{
	SFSE::Init(sfse);
	SFSE::GetMessagingInterface()->RegisterListener(MessageCallback);
	return true;
}

extern "C" __declspec(dllexport) bool OSFBrowser_Open() { return OSF::API::OpenBrowser(); }
extern "C" __declspec(dllexport) bool OSFBrowser_OpenWheel(const char* prefix) { return OSF::API::OpenWheel(prefix ? prefix : ""); }
extern "C" __declspec(dllexport) bool OSFBrowser_IsVisible() { return OSF::API::BrowserVisible(); }
