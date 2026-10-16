#include "Scene/SceneRuntime.h"

#include "Matchmaking/Matchmaker.h"
#include "Scene/AnchorResolve.h"
#include "Scene/SceneEventRelay.h"

namespace OSF::Scene
{
	bool SceneRuntime::Replace(std::int32_t a_scene, std::string_view a_id,
		const std::vector<std::string>& a_roles, const ReplaceOptions& a_options)
	{
		SlotView old;
		Registry::SceneRef next;
		std::vector<RE::Actor*> cast;
		std::string entry;
		WorldAnchor anchor, base, oldAnchor, oldBase;
		RE::NiPointer<RE::TESObjectREFR> ref, oldRef;
		Alignment::WorkingCopy oldAlignment;
		std::size_t oldSelected = 0;
		{
			std::lock_guard l{ _lock };
			auto* s = Resolve(a_scene);
			if (!s || s->transitioning) return false;
			const auto refuse = [&](std::string_view reason) {
				REX::WARN("[Scene] handoff {:#010x} '{}' -> '{}' refused: {}", a_scene, s->id, a_id, reason);
				return false;
			};
			next = Registry::ContentRegistry::GetSingleton().Find(a_id);
			if (!next) return refuse("unknown scene");
			std::string error;
			if (!Matchmaking::BindSceneRoles(*next, s->participants, a_roles, cast, error)) return refuse(error);
			const auto placement = s->worldPlacement.value_or(s->definition ? s->definition->worldPlacement : Animation::WorldPlacementMode::kAnchorAndPin);
			if (placement != next->worldPlacement) return refuse("follow-actor and anchored scenes cannot be mixed");
			entry = next->entry;
			if (a_options.stage > 0 && !next->linearStages.empty()) {
				if (static_cast<std::size_t>(a_options.stage) >= next->linearStages.size()) return refuse("stage out of range");
				entry = next->linearStages[a_options.stage];
			}
			if (!next->FindNode(entry)) return refuse("entry node is missing");
			if (a_options.headingRad && !std::isfinite(*a_options.headingRad)) return refuse("non-finite anchor heading");
			anchor = s->anchor;
			base = s->anchorBase;
			ref = s->anchorRef;
			if (a_options.anchor) {
				if (a_options.anchor != ref.get()) base = MakeAnchorAt(a_options.anchor, std::nullopt);
				ref.reset(a_options.anchor);
			}
			if (a_options.headingRad && base.set) base.heading = *a_options.headingRad;
			if (next->RequiresAnchor()) {
				if (!ref || !base.set || !Matchmaking::AnchorAccepts(*next, ref.get()))
					return refuse("missing or incompatible anchor reference");
				anchor = ComposeSceneAnchor(base, next->anchorOffset);
			} else if (a_options.anchor) {
				anchor = base;
			}
			old = { s->id, s->node, s->participants, s->playbackId, s->definition, s->cameraOverridden };
			oldAnchor = s->anchor; oldBase = s->anchorBase; oldRef = s->anchorRef;
			s->transitioning = true;
		}

		// Exit lanes retain their authored precedence and still observe the old identity and cast.
		Fire(a_scene, Event::kNodeExit, old.node, "exit");
		{
			std::lock_guard l{ _lock };
			auto* s = Resolve(a_scene);
			if (!s) return false;  // a world reset from an external callback discarded the slot
			s->definition = next; s->id = next->id; s->node = entry; s->participants = cast;
			s->anchor = anchor; s->anchorBase = base; s->anchorRef = ref;
			oldAlignment = std::move(s->align); s->align = {};
			oldSelected = s->alignSelected; s->alignSelected = 0;
		}
		const auto playback = PlayNodeAnim(cast, next->id, entry, old.playbackId);
		if (!playback) {
			{
				std::lock_guard l{ _lock };
				auto* s = Resolve(a_scene);
				if (!s) return false;
				s->definition = old.definition; s->id = old.id; s->node = old.node; s->participants = old.participants;
				s->anchor = oldAnchor; s->anchorBase = oldBase; s->anchorRef = oldRef;
				s->align = std::move(oldAlignment); s->alignSelected = oldSelected;
				s->terminationCause = TerminationCause::kInterrupted;
			}
			StopGraph(old.participants, old.playbackId);
			Fire(a_scene, Event::kSceneEnd, old.node, "");
			REX::INFO("[Scene] end id='{}' handle={:#010x} cause=interrupted (handoff to '{}' failed)", old.id, a_scene, next->id);
			ReleaseSlot(a_scene);
			return false;
		}
		{
			std::lock_guard l{ _lock };
			auto* s = Resolve(a_scene);
			if (!s) return false;
			s->playbackId = playback;
		}
		// Add the new defaults without undoing carried mechanisms or adding a runtime fade.
		EngageDefaultPlayerInputLock(a_scene, next->playerInputLock, cast);
		if (!old.cameraOverridden) EngageDefaultCamera(a_scene, entry, next->playerInputLock, {}, cast);
		const auto gear = BuildGearPicks(a_scene, cast);
		ClearHeldItems(a_scene, next->clearHeldItems, cast, gear);
		HideDefaultApparel(a_scene, next->hideApparel, cast, gear);
		EquipGearItems(a_scene, cast, gear);
		EquipRoleItems(a_scene, next->id, cast);
		EngageDefaultSceneControls(a_scene, next->id, std::nullopt, cast);
		Fire(a_scene, Event::kSceneHandoff, entry, "", old.id);
		SlotView live;
		if (!SnapshotSlot(a_scene, live)) return true;  // a callback replaced the world
		Fire(a_scene, Event::kNodeEnter, entry, "enter");
		EndTransition(a_scene);
		REX::DEBUG("[Scene] handoff handle={:#010x} '{}' -> '{}' node='{}'", a_scene, old.id, next->id, entry);
		return true;
	}

	bool SceneRuntime::ReplaceByTags(std::int32_t a_scene, const Matchmaking::TagQuery& a_query)
	{
		std::vector<RE::Actor*> cast;
		std::string current;
		RE::NiPointer<RE::TESObjectREFR> anchor;
		Animation::WorldPlacementMode placement;
		{
			std::lock_guard l{ _lock };
			const auto* s = Resolve(a_scene);
			if (!s || s->transitioning) return false;
			cast = s->participants; current = s->id; anchor = s->anchorRef;
			placement = s->worldPlacement.value_or(s->definition ? s->definition->worldPlacement : Animation::WorldPlacementMode::kAnchorAndPin);
		}
		const auto picked = Matchmaking::PickReplacement(cast, a_query, anchor.get(), current, placement);
		if (!picked) return false;
		const auto def = Registry::ContentRegistry::GetSingleton().Find(picked->id);
		if (!def || def->roles.size() != cast.size() || picked->order.size() != cast.size()) return false;
		std::vector<std::string> roles(cast.size());
		for (std::size_t i = 0; i < picked->order.size(); ++i) {
			if (picked->order[i] >= roles.size()) return false;
			roles[picked->order[i]] = def->roles[i].name;
		}
		return Replace(a_scene, picked->id, roles, {});
	}
}
