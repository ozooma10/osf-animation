#include "Scene/SceneRuntime.h"

#include "Props/PropService.h"

#include <algorithm>

namespace OSF::Scene
{
	void SceneRuntime::AttachSceneProp(
		std::int32_t a_handle, const Registry::ActionEntry& a_action)
	{
		auto* actor = ResolveRoleActor(a_handle, a_action.role);
		if (!actor) {
			REX::DEBUG("[Scene] scene {:#010x} osf.prop.attach '{}' — role '{}' resolved no actor",
				a_handle, a_action.prop, a_action.role);
			return;
		}

		// Reattaching a named live prop changes only its node/transform. Copying
		// the NiPointers keeps the object alive while the service runs outside
		// the scene-table lock; the slot remains its logical owner.
		Props::Instance existing;
		{
			std::lock_guard l{ _lock };
			Slot* slot = Resolve(a_handle);
			if (!slot) {
				return;
			}
			const auto it = std::find_if(
				slot->props.begin(), slot->props.end(),
				[&](const ActiveProp& a_prop) {
					return a_prop.id == a_action.prop;
				});
			if (it != slot->props.end()) {
				existing = it->instance;
			}
		}

		std::string error;
		auto& service = Props::PropService::GetSingleton();
		if (!existing.Empty()) {
			if (!service.Attach(existing, a_action.propAttachment, &error)) {
				REX::WARN("[Scene] scene {:#010x} prop reattach '{}' failed: {}",
					a_handle, a_action.prop, error);
			}
			return;
		}

		auto created = service.CreateAttached(
			actor, a_action.propSource, a_action.propAttachment, &error);
		if (created.Empty()) {
			REX::WARN("[Scene] scene {:#010x} prop create '{}' failed: {}",
				a_handle, a_action.prop, error);
			return;
		}
		const auto sourceForm = created.sourceForm;

		bool recorded = false;
		{
			std::lock_guard l{ _lock };
			Slot* slot = Resolve(a_handle);
			if (slot) {
				const auto duplicate = std::find_if(
					slot->props.begin(), slot->props.end(),
					[&](const ActiveProp& a_prop) {
						return a_prop.id == a_action.prop;
					});
				if (duplicate == slot->props.end()) {
					slot->props.push_back(
						ActiveProp{ a_action.prop, std::move(created) });
					if (std::find(
							slot->ledger.begin(), slot->ledger.end(),
							Mechanism::kProps) == slot->ledger.end()) {
						slot->ledger.push_back(Mechanism::kProps);
					}
					recorded = true;
				}
			}
		}
		if (!recorded) {
			if (!service.Destroy(created, &error)) {
				REX::ERROR("[Scene] scene {:#010x} unrecorded prop '{}' cleanup failed: {}",
					a_handle, a_action.prop, error);
			}
			return;
		}

		REX::DEBUG("[Scene] scene {:#010x} attached prop '{}' (form {:08X}) to '{}'",
			a_handle, a_action.prop, sourceForm, a_action.propAttachment.node);
	}

	void SceneRuntime::DestroySceneProp(
		std::int32_t a_handle, std::string_view a_prop)
	{
		Props::Instance instance;
		{
			std::lock_guard l{ _lock };
			Slot* slot = Resolve(a_handle);
			if (!slot) {
				return;
			}
			const auto it = std::find_if(
				slot->props.begin(), slot->props.end(),
				[&](const ActiveProp& a_live) {
					return a_live.id == a_prop;
				});
			if (it == slot->props.end()) {
				return;
			}
			instance = std::move(it->instance);
			slot->props.erase(it);
			if (slot->props.empty()) {
				const auto ledger = std::find(
					slot->ledger.begin(), slot->ledger.end(),
					Mechanism::kProps);
				if (ledger != slot->ledger.end()) {
					slot->ledger.erase(ledger);
				}
			}
		}

		std::string error;
		if (!Props::PropService::GetSingleton().Destroy(instance, &error)) {
			REX::ERROR("[Scene] scene {:#010x} prop destroy '{}' failed: {}",
				a_handle, a_prop, error);
			return;
		}
		REX::DEBUG("[Scene] scene {:#010x} destroyed prop '{}'", a_handle, a_prop);
	}
}
