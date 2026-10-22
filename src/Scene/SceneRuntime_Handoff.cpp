#include "Scene/SceneRuntime.h"

#include "Animation/GraphManager.h"
#include "Matchmaking/Matchmaker.h"
#include "Scene/AnchorResolve.h"
#include "Scene/SceneEventRelay.h"

namespace OSF::Scene
{
	bool SceneRuntime::Replace(std::int32_t handle, std::string_view id,
		const std::vector<std::string>& roles, const ReplaceOptions& options)
	{
		return AdmitTransition(handle, id, roles, options, TransitionOrigin::Caller);
	}

	bool SceneRuntime::AdmitTransition(std::int32_t handle, std::string_view id,
		const std::vector<std::string>& roles, const ReplaceOptions& options,
		TransitionOrigin origin, const SlotView* expected)
	{
		auto p = std::make_shared<PreparedTransition>();
		p->context.scene = handle;
		p->context.kind = Preparation::Kind::Transition;
		p->context.origin = origin;
		p->context.toNode = id;
		p->context.toStage = options.stage;
		p->automatic = IsAutomaticTransition(origin);
		p->covered = options.covered;
		{
			std::lock_guard l{ _lock };
			auto* s = Resolve(handle);
			if (!s || s->preparation || s->cover) return false;
			if (expected) {
				if (!s->transitioning || s->playbackId != expected->playbackId ||
					s->node != expected->node || s->definition.get() != expected->definition.get()) return false;
			} else if (s->transitioning) {
				return false;
			}
			p->old = { s->node, s->participants, s->playbackId, s->definition, s->cameraOverridden };
			p->cast = s->participants;
			p->context.fromNode = s->node;
			p->anchor = s->anchor;
			p->anchorBase = s->anchorBase;
			p->anchorRef = s->anchorRef;
			p->overrides.playerInputLock = s->playerInputLockOverride;
			p->overrides.hideApparel = s->hideApparelOverride;
			p->overrides.sceneControls = s->sceneControlsOverride;
			auto& registry = Registry::ContentRegistry::GetSingleton();
			// Authored edges stay within the source's immutable publication across reloads.
			// Exact requests resolve the currently published destination through the same admission.
			const auto next = registry.Find(id, expected ? s->definition : Registry::NodeRef{});
			const auto refuse = [&](std::string reason) { p->failureReason = std::move(reason); };
			if (!next) {
				refuse("unknown destination node");
			} else {
				p->context.toNode = next->id;
				std::vector<RE::Actor*> cast;
				std::string error;
				const auto placement = s->worldPlacement.value_or(s->definition ? s->definition->worldPlacement : Animation::WorldPlacementMode::kAnchorAndPin);
				if (!Matchmaking::BindSceneRoles(*next, s->participants, roles, cast, error)) {
					refuse(error);
				} else if (placement != s->worldPlacement.value_or(next->worldPlacement)) {
					refuse("follow-actor and anchored nodes cannot be mixed");
				} else if (options.stage < 0 || static_cast<std::size_t>(options.stage) >= next->stages.size()) {
					refuse("stage out of range");
				} else if (options.headingRad && !std::isfinite(*options.headingRad)) {
					refuse("non-finite anchor heading");
				} else {
					if (options.anchor) {
						if (options.anchor != p->anchorRef.get()) p->anchorBase = MakeAnchorAt(options.anchor, std::nullopt);
						p->anchorRef.reset(options.anchor);
					}
					if (options.headingRad && p->anchorBase.set) p->anchorBase.heading = *options.headingRad;
					if (next->RequiresAnchor()) {
						if (!p->anchorRef || !p->anchorBase.set || !Matchmaking::AnchorAccepts(*next, p->anchorRef.get()))
							refuse("missing or incompatible anchor reference");
						else
							p->anchor = ComposeSceneAnchor(p->anchorBase, next->anchorOffset);
					} else if (p->anchorBase.set) {
						p->anchor = p->anchorBase;
					}
					if (p->failureReason.empty()) p->cast = std::move(cast);
				}
				if (p->failureReason.empty()) {
					p->target = registry.ResolveLoopGlobals(next);
					p->defaults = true;
					p->playerInputLock = p->overrides.playerInputLock.value_or(next->playerInputLock);
					p->hideApparel = p->overrides.hideApparel.value_or(next->hideApparel);
					p->clearHeldItems = next->clearHeldItems;
				}
			}
			s->transitioning = true;
		}
		if (!p->old.participants.empty()) {
			if (const auto current = Animation::GraphManager::GetSingleton().GetScenePlayback(p->old.participants.front(), p->old.playbackId))
				p->context.fromStage = current->stage;
		}
		// Admission refusals use the same terminal/resume policy as preparation vetoes.
		// An automatic source already consumed its boundary and must not be stranded.
		if (!QueuePreparation(p)) {
			std::lock_guard l{ _lock };
			if (auto* s = Resolve(handle); s && !s->preparation && !s->cover &&
				s->playbackId == p->old.playbackId && s->node == p->old.node) s->transitioning = false;
			return false;
		}
		return true; // accepted attempt; ReadPreparation reports its eventual result
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
			cast = s->participants; current = s->node; anchor = s->anchorRef;
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
