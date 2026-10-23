#include "Packs/PackReload.h"
#include "Profiling/Profile.h"

#include "API/Health.h"
#include "API/EngineServices.h"
#include "Equipment/GearRegistry.h"
#include "Scene/AlignmentStore.h"
#include "Registry/ContentRegistry.h"
#include "Registry/SoundRegistry.h"
#include "Serialization/AFImport.h"
#include "Serialization/GLTFImport.h"

namespace OSF::Content
{
	std::int32_t ReloadAll()
	{
		OSF_PROFILE_SCOPE("Registry.ReloadAll");
		Serialization::GLTFImport::ClearCache();
		Serialization::AFImport::ClearCache();
		REX::DEBUG("[Registry] content reload: clip caches cleared");

		auto& registry = Registry::ContentRegistry::GetSingleton();
		registry.LoadAll();
		Registry::SoundRegistry::GetSingleton().LoadAll();
		Equipment::Gear::LoadAll();
		Scene::Alignment::Load();
		API::Health::ReportRegistryLoad();

		API::EngineServices::ContentReloaded();
		return static_cast<std::int32_t>(registry.Size());
	}
}
