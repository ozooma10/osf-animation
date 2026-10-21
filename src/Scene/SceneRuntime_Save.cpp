#include "Scene/SceneRuntime.h"

namespace OSF::Scene
{
	void SceneRuntime::OnSaveBegin() { SetEquipmentSaveWindow(true); }
	void SceneRuntime::OnSaveEnd() { SetEquipmentSaveWindow(false); }
	void SceneRuntime::SetEquipmentSaveWindow(bool saving)
	{
		_preparationSaving = saving;
		if (saving) {
			std::lock_guard l{ _lock };
			for (auto& slot : _slots)
				if (slot.preparation)
					slot.preparation->cancelled = true;
		}
		// The existing save lifecycle executes in game context. Persistent owners stay active;
		// only session demands are suspended, without consuming their claims.
		ActorState::Report(ActorState::Get().SetSaveWindow(saving), saving ? "suspend for save" : "resume after save");
	}
}
