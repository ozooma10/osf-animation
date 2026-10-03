#include "Packs/PackReload.h"

#include "API/Health.h"
#include "API/UIBridge.h"
#include "Equipment/GearRegistry.h"
#include "Registry/ContentRegistry.h"
#include "Registry/SoundRegistry.h"
#include "Serialization/AFImport.h"
#include "Serialization/ClipDurations.h"
#include "Serialization/GLTFImport.h"

namespace OSF::Content
{
	std::int32_t ReloadAll()
	{
		Serialization::GLTFImport::ClearCache();
		Serialization::AFImport::ClearCache();
		REX::DEBUG("[Registry] content reload: clip caches cleared");

		auto& registry = Registry::ContentRegistry::GetSingleton();
		registry.LoadAll();
		Registry::SoundRegistry::GetSingleton().LoadAll();
		Equipment::Gear::LoadAll();
		API::Health::ReportRegistryLoad();

		// Edited files miss the size/mtime duration cache; the async scan pushes the refreshed catalog.
		Serialization::ClipDurations::ScanSceneClipsAsync(&API::PushCatalogUpdate);
		return static_cast<std::int32_t>(registry.Size());
	}
}
