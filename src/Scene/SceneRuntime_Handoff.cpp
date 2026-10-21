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
		WorldAnchor                      anchor, base;
		RE::NiPointer<RE::TESObjectREFR> ref;
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
			s->transitioning = true;
		}

		auto p = std::make_shared<PreparedTransition>();
		p->context.scene = a_scene;
		p->context.kind = Preparation::Kind::Handoff;
		p->context.fromScene = old.id;
		p->context.fromNode = old.node;
		p->context.toScene = next->id;
		p->context.toNode = entry;
		p->context.fromStage = old.definition ? old.definition->LinearStageOf(old.node) : -1;
		p->context.toStage = next->LinearStageOf(entry);
		p->old = old;
		p->cast = cast;
		p->target = Registry::ContentRegistry::GetSingleton().ResolveLoopGlobals(next);
		p->anchor = anchor;
		p->anchorBase = base;
		p->anchorRef = ref;
		p->defaults = true;
		p->playerInputLock = next->playerInputLock;
		p->hideApparel = next->hideApparel;
		p->clearHeldItems = next->clearHeldItems;
		if (!QueuePreparation(p)) {
			EndTransition(a_scene);
			return false;
		}
		return true;  // accepted; successful handoff notification follows preparation and playback commit
	}

	bool SceneRuntime::ReplaceByQuery(std::int32_t a_scene, const Matchmaking::TagQuery& a_query,
		const Matchmaking::RoleConstraints& a_roles, RE::TESObjectREFR* a_anchor, Matchmaking::AnchorMode a_mode)
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
		if (!Matchmaking::RoleConstraintsFit(a_roles, cast.size())) {
			REX::WARN("[Scene] handoff {:#010x} refused: {} role constraint(s) for {} participant(s)", a_scene, a_roles.size(), cast.size());
			return false;
		}
		if (a_anchor) anchor.reset(a_anchor);
		const auto picked = Matchmaking::PickReplacement(cast, a_query, anchor.get(), current, placement, a_roles, a_mode);
		if (!picked) return false;
		const auto def = Registry::ContentRegistry::GetSingleton().Find(picked->id);
		if (!def || def->roles.size() != cast.size()) return false;
		const auto roleOf = Matchmaking::RoleOfActor(picked->order, cast.size());
		if (roleOf.empty()) return false;
		// Replace binds by name. Duplicate names (e.g. two anonymous roles) are ambiguous, so those scenes hand off
		// positionally, which only works when the pick kept the current order.
		std::vector<std::string> declared, roles(cast.size());
		for (const auto& role : def->roles) declared.push_back(role.name);
		for (std::size_t i = 0; i < cast.size(); ++i) roles[i] = declared[roleOf[i]];
		if (!Matchmaking::RoleNamesUnique(declared)) {
			std::vector<RE::Actor*> ordered(cast.size());
			for (std::size_t role = 0; role < cast.size(); ++role) ordered[role] = cast[picked->order[role]];
			if (ordered != cast) {
				REX::WARN("[Scene] handoff {:#010x} -> '{}' refused: anonymous roles cannot be re-bound to a new order", a_scene, picked->id);
				return false;
			}
			roles.clear();  // positional: the current order already fits
		}
		SceneRuntime::ReplaceOptions options;
		options.anchor = a_anchor;
		return Replace(a_scene, picked->id, roles, options);
	}
}
