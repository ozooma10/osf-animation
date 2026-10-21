#include "Animation/GraphManager.h"
#include "Equipment/EquipmentService.h"
#include "Scene/SceneEventRelay.h"
#include "Scene/SceneRuntime.h"
#include "Util/FormRef.h"
#include "Util/MonotonicClock.h"
#include "Util/NativeMainThreadQueue.h"
#include "Util/StringUtil.h"

namespace OSF::Scene
{
	using namespace Preparation;
	namespace
	{
		std::uint64_t NowMs() { return Util::MonotonicNs() / 1000000; }
	}
	Token SceneRuntime::RegisterPreparer(Provider provider) { return Util::NativeMainThreadQueue::IsDraining() ? _preparers.Register(std::move(provider)) : 0; }
	bool  SceneRuntime::UnregisterPreparer(Token provider)
	{
		if (!Util::NativeMainThreadQueue::IsDraining() || !_preparers.Unregister(provider))
			return false;
		std::vector<std::int32_t> scenes;
		{
			std::lock_guard l{ _lock };
			for (std::uint16_t i = 0; i < _slots.size(); ++i)
				if (_slots[i].generation && !_slots[i].ended && (_slots[i].preparedScopes.contains(provider) || (_slots[i].preparation && _slots[i].preparation->scopes.contains(provider))))
					scenes.push_back(MakeToken(_slots[i].generation, i));
		}
		for (auto scene : scenes) Stop(scene);
		return true;
	}
    bool SceneRuntime::GuardPreparation(Token attempt, Token provider, std::function<bool(const Context&)> acquire, std::function<void(const Context&)> release)
    {
        if (!Util::NativeMainThreadQueue::IsDraining()) return false;
        const auto it = _preparations.find(attempt);
        if (it == _preparations.end()) return false;
        const auto p = it->second;
        return _preparers.Guard(attempt,provider,{[p,acquire=std::move(acquire)] { return acquire(p->context); },[p,release=std::move(release)] { release(p->context); }});
    }
	bool SceneRuntime::PlanEquipment(Token attempt, Token provider, const ActorState::Request& request)
	{
		if (!Util::NativeMainThreadQueue::IsDraining() || _preparers.CurrentProvider(attempt, Phase::Plan) != provider)
			return false;
		const auto it = _preparations.find(attempt);
		if (it == _preparations.end() || std::find(it->second->context.actors.begin(), it->second->context.actors.end(), request.actor) == it->second->context.actors.end())
			return false;
		auto copy = request;
		copy.group = 0;
		auto& requests = it->second->requests[provider];
		// Plan may be polled; exact duplicate requirements do not accumulate.
		if (std::none_of(requests.begin(), requests.end(), [&](const auto& r) { return r.actor == copy.actor && r.object == copy.object && r.operation == copy.operation && r.allowAdd == copy.allowAdd; }))
			requests.push_back(copy);
		return true;
	}
	bool SceneRuntime::ReserveResource(Token attempt, Token provider, std::string_view key)
	{
		if (!Util::NativeMainThreadQueue::IsDraining() || key.empty() || key.size() > 256 || _preparers.CurrentProvider(attempt, Phase::Plan) != provider)
			return false;
		const auto it = _preparations.find(attempt);
		if (it == _preparations.end())
			return false;
		auto&      p = *it->second;
		const auto lease = _resources.find(std::string(key));
		if (lease != _resources.end() && (lease->second.scene != p.context.scene || lease->second.provider != provider))
			return false;
		p.resources[std::string(key)] = provider;
		// Existing committed reservations stay committed until this transition succeeds.
		if (lease == _resources.end())
			_resources.emplace(key, ResourceLease{ p.context.scene, provider, attempt });
		return true;
	}
	bool SceneRuntime::PreparationFailure(Token attempt, Token provider, std::string_view reason)
	{
		if (!Util::NativeMainThreadQueue::IsDraining() || _preparers.CurrentProvider(attempt) != provider || reason.size() > 1024)
			return false;
		const auto it = _preparations.find(attempt);
		if (it == _preparations.end())
			return false;
		it->second->failureReason = reason;
		return true;
	}
	std::string SceneRuntime::ReadPreparation(std::int32_t scene)
	{
		std::lock_guard l{ _lock };
		const auto*     s = Resolve(scene, true);
		if (!s)
			return {};
		nlohmann::json result = { { "scene", scene }, { "status", s->preparationResult }, { "reason", s->preparationError }, { "pending", static_cast<bool>(s->preparation) }, { "playing", !s->ended && s->playbackId != 0 } };
		if (s->preparation)
			result["attempt"] = s->preparation->context.attempt;
		return result.dump();
	}
	bool SceneRuntime::PreparedEquipmentAction(Registry::ActionKind kind)
	{
		using K = Registry::ActionKind;
		return kind == K::kEquipmentEquip || kind == K::kEquipmentUnequip || kind == K::kEquipmentHide || kind == K::kEquipmentRestore;
	}
	bool SceneRuntime::PreparationValid(const std::shared_ptr<PreparedTransition>& p)
	{
		std::lock_guard l{ _lock };
		const auto*     slot = Resolve(p->context.scene);
		return !p->cancelled && !p->reset && !_preparationSaving && slot && slot->preparation == p && slot->playbackId == (p->committed ? p->published : p->old.playbackId);
	}
	bool SceneRuntime::QueuePreparation(const std::shared_ptr<PreparedTransition>& p)
	{
		for (auto* actor : p->cast) {
			if (!actor)
				return false;
			p->pins.emplace_back(actor);
			p->context.actors.push_back(actor->GetFormID());
		}
		{
			std::lock_guard l{ _lock };
			auto*           s = Resolve(p->context.scene);
			if (!s || s->preparation)
				return false;
			s->transitioning = true;
			s->preparation = p;
			s->preparationResult = "pending";
			s->preparationError.clear();
		}
		Util::NativeMainThreadQueue::Post([this, p] { StartPreparation(p); });
		return true;
	}
	void SceneRuntime::StartPreparation(const std::shared_ptr<PreparedTransition>& p)
	{
		if (!PreparationValid(p)) {
			FinishPreparation(p, p->reset ? Outcome::WorldReset : Outcome::Cancelled, false);
			return;
		}
		auto& gm = Animation::GraphManager::GetSingleton();
		if (p->old.playbackId && !gm.HoldForPreparation(p->old.participants.front(), p->old.playbackId, true)) {
			p->interrupted = true;
			p->cancelled = true;
			FinishPreparation(p, Outcome::Failed, false);
			return;
		}
		Hooks hooks;
		hooks.valid = [this, p] { return PreparationValid(p); };
		hooks.invalidReason = [p] { return p->reset ? Outcome::WorldReset : p->interrupted ? Outcome::Failed : Outcome::Cancelled; };
		hooks.preflight = [this, p] { return BuildPreparationPlan(p); };
		hooks.apply = [this, p] {
			// Exit effects precede entry equipment. A callback can cancel/reset before any target mutation.
			if (p->context.kind != Kind::Start && p->context.kind != Kind::Stage) {
				p->exitFired = true;
				Fire(p->context.scene, Event::kNodeExit, p->old.node, "exit");
				if (!PreparationValid(p))
					return false;
			}
			std::vector<ActorState::Handle> scopes{ EquipmentScope(p->context.scene) };
			for (const auto& [provider, scope] : p->scopes) scopes.push_back(scope);
			p->edit = ActorState::Get().BeginEdit(scopes);
			if (!p->edit)
				return false;
			p->applied = true;
			const auto result = ActorState::Get().ApplyPlan(p->changes);
			if (result != ActorState::Status::Ok && p->failureReason.empty())
				p->failureReason = "equipment application failed (status " + std::to_string(static_cast<unsigned>(result)) + ")";
			return result == ActorState::Status::Ok;
		};
		hooks.ready = [p] {
			for (auto* actor : p->cast) {
				{
					const auto loaded = actor->loadedData.LockRead();
					if (!*loaded || !(*loaded)->data3D)
						return Decision::Pending;
				}
				if (ActorState::Get().Reconcile(actor->GetFormID()) != ActorState::Status::Ok)
					return Decision::Veto;
			}
            p->validatedRevisions.clear();
            for (auto id : p->context.actors) p->validatedRevisions.emplace(id, ActorState::Get().ReadRevision(id));
			return Decision::Approve;
		};
        hooks.beforeCommit = [p] {
            for (auto* actor : p->cast) {
                {
                    const auto loaded = actor->loadedData.LockRead();
                    if (!*loaded || !(*loaded)->data3D) { p->failureReason="actor-unloaded-before-commit"; return false; }
                }
                if (ActorState::Get().Reconcile(actor->GetFormID()) != ActorState::Status::Ok) { p->failureReason="equipment-reconcile-before-commit-failed"; return false; }
            }
            return true;
        };
        hooks.checkCommit = [p] {
            for (const auto& [id, revision] : p->validatedRevisions)
                if (ActorState::Get().ReadRevision(id) != revision) { p->failureReason="actor-state-changed-after-validate"; return false; }
            return p->validatedRevisions.size() == p->cast.size();
        };
		hooks.commit = [this, p] { return CommitPreparation(p); };
		hooks.finish = [this, p](Outcome outcome, bool applied) { FinishPreparation(p, outcome, applied); };
		const auto id = _preparers.Begin(p->context, std::move(hooks), NowMs());
		if (!id) {
			FinishPreparation(p, Outcome::Failed, false);
			return;
		}
		p->context.attempt = id;
		_preparations.emplace(id, p);
		PollPreparation();
	}
	void SceneRuntime::PollPreparation()
	{
		if (_preparationPollQueued || _preparers.PendingCount() == 0)
			return;
		_preparationPollQueued = true;
		Util::NativeMainThreadQueue::Post([this] {
			_preparationPollQueued = false;
			if (!_preparationSaving)
				_preparers.Tick(NowMs());
			PollPreparation();
		});
	}
	bool SceneRuntime::BuildPreparationPlan(const std::shared_ptr<PreparedTransition>& p)
	{
		if (!PreparationValid(p))
			return false;
		auto&      state = ActorState::Get();
		auto&      backend = Equipment::EquipmentService::GetSingleton();
		const auto scope = EquipmentScope(p->context.scene);
		if (!scope)
			return false;
		{
			std::lock_guard l{ _lock };
			auto*           slot = Resolve(p->context.scene);
			if (!slot)
				return false;
			p->scopes = slot->preparedScopes;
		}
		for (const auto provider : _preparers.Providers(p->context.attempt))
			if (!p->scopes.contains(provider)) {
				auto created = state.CreateScope("osf.preparer." + std::to_string(provider), ActorState::Lifetime::Session, static_cast<std::uint32_t>(p->context.scene));
				if (!created)
					return false;
				p->scopes[provider] = created;
				p->newScopes.push_back(created);
			}
		p->changes.clear();
		// Release only the previous destination's provider demands, leaving persistent devices intact.
		for (const auto& [provider, owned] : p->scopes) p->changes.push_back({ owned, { 0, 0, ActorState::Operation::Equip, 0 }, true, false, false, {} });

		const auto addEquip = [&](RE::Actor* actor, RE::TESBoundObject* object, bool gear) {
			if (!actor || !object)
				return false;
			p->changes.push_back({ scope, { actor->GetFormID(), object->GetFormID(), ActorState::Operation::Equip, gear ? ActorState::Gear : ActorState::Authored, !gear }, false, !gear, gear, gear ? std::nullopt : std::optional<std::uint32_t>{ ActorState::Gear } });
			return true;
		};
		const auto hide = [&](RE::Actor* actor, bool held) {
			const auto inventory = backend.Read(actor->GetFormID());
			if (!inventory)
				return false;
			const auto hidden = state.Hidden(actor->GetFormID());
			for (const auto& item : *inventory) {
				if (item.skin || !item.count || (!item.worn && std::find(hidden.begin(), hidden.end(), item.object) == hidden.end()))
					continue;
				if (held ? item.armor : !(item.armor || item.weapon))
					continue;
				p->changes.push_back({ scope, { actor->GetFormID(), item.object, ActorState::Operation::HideOptional, held ? ActorState::Held : ActorState::Apparel, false }, false, false, true, {} });
			}
			return true;
		};
		const auto actions = [&](const Registry::SceneRef& def, std::string_view nodeId, const std::vector<RE::Actor*>& cast, Registry::ActionPos position) {
			const auto* node = def ? def->FindNode(nodeId) : nullptr;
			if (!node)
				return true;
			for (const auto& action : node->actions) {
				if (action.pos != position || !PreparedEquipmentAction(action.kind))
					continue;
				RE::Actor* actor = nullptr;
				if (action.role.empty() && !cast.empty())
					actor = cast.front();
				else
					for (std::size_t i = 0; i < def->roles.size() && i < cast.size(); ++i)
						if (Util::ToLower(def->roles[i].name) == Util::ToLower(action.role)) {
							actor = cast[i];
							break;
						}
				using K = Registry::ActionKind;
				if (action.kind == K::kEquipmentEquip) {
					if (!addEquip(actor, Util::ResolveBoundObject(action.item), false))
						return false;
				} else if (action.kind == K::kEquipmentHide) {
					if (!actor || !hide(actor, false))
						return false;
				} else {
					const auto group = action.kind == K::kEquipmentRestore ? ActorState::Apparel : ActorState::Authored;
					p->changes.push_back({ scope, { 0, 0, ActorState::Operation::Equip, group }, true, false, false, {} });
				}
			}
			return true;
		};
		if (p->context.kind != Kind::Start && p->context.kind != Kind::Stage && !actions(p->old.definition, p->old.node, p->old.participants, Registry::ActionPos::kExit))
			return false;
		// Required consumer claims precede optional gear and hides. Prior claims are released
		// only by explicit exit actions or by the same provider's destination replacement.
		for (const auto& [provider, requests] : p->requests)
			for (const auto& r : requests) p->changes.push_back({ p->scopes.at(provider), r, false, false, false, {} });
		if (p->defaults) {
			for (std::size_t i = 0; i < p->cast.size(); ++i) {
				auto*                    actor = p->cast[i];
				std::vector<std::string> taken;
				RE::TESBoundObject*      authored = nullptr;
				if (p->target && i < p->target->roles.size()) {
					const auto& role = p->target->roles[i];
					const auto& ref = role.equip.ForGender(Matchmaking::ActorGenderTag(actor));
					if (!ref.empty()) {
						authored = Util::ResolveBoundObject(ref);
						if (!authored)
							return false;
						const auto gearSlot = Equipment::Gear::SlotOfForm(authored->GetFormID());
						if (!gearSlot.empty())
							taken.push_back(gearSlot);
					}
				}
				// Claim gear before optional hides, including pre-worn gear.
				if (Equipment::Gear::AutoEquip())
					for (const auto& pick : Equipment::Gear::SelectForActor(actor, taken))
						if (pick.object)
							addEquip(actor, pick.object, true);
				if (authored && !addEquip(actor, authored, false))
					return false;
				if (p->clearHeldItems && !hide(actor, true))
					return false;
				if (p->hideApparel && !hide(actor, false))
					return false;
			}
		}
		if (p->context.kind != Kind::Stage && !actions(p->target, p->context.toNode, p->cast, Registry::ActionPos::kEnter))
			return false;
		const auto result = state.PreviewPlan(p->changes);
		if (result != ActorState::Status::Ok && p->failureReason.empty())
			p->failureReason = "equipment preflight refused (status " + std::to_string(static_cast<unsigned>(result)) + ")";
		return result == ActorState::Status::Ok;
	}
	bool SceneRuntime::CommitPreparation(const std::shared_ptr<PreparedTransition>& p)
	{
		if (!PreparationValid(p))
			return false;
		const auto handle = p->context.scene;
		if (p->context.kind == Kind::Stage) {
			if (!Animation::GraphManager::GetSingleton().CommitSceneStage(p->cast.front(), p->context.toStage, p->old.playbackId))
				return false;
			p->published = p->old.playbackId;
		} else {
			{
				std::lock_guard l{ _lock };
				auto*           s = Resolve(handle);
				if (!s || s->preparation != p)
					return false;
				s->id = p->context.toScene;
				s->node = p->context.toNode;
				s->participants = p->cast;
				s->definition = p->target;
				if (p->context.kind == Kind::Handoff) {
					s->anchor = p->anchor;
					s->anchorBase = p->anchorBase;
					s->anchorRef = p->anchorRef;
					s->align = {};
					s->alignSelected = 0;
				}
			}
			if (p->plan) {
				auto plan = *p->plan;
				{
					std::lock_guard l{ _lock };
					const auto*     s = Resolve(handle);
					if (!s)
						return false;
					s->speed.Apply(plan);
				}
				if (!Animation::GraphManager::GetSingleton().PlaySynchronized(p->cast, plan, p->context.toStage, &p->published, 0, _playbackSinkId))
					return false;
			} else
				p->published = PlayNodeAnim(p->cast, p->context.toScene, p->context.toNode, p->old.playbackId);
			if (!p->published)
				return false;
			const auto      playback = Animation::GraphManager::GetSingleton().GetScenePlayback(p->cast.front(), p->published);
			std::lock_guard l{ _lock };
			auto*           s = Resolve(handle);
			if (!s || s->preparation != p)
				return false;
			s->playbackId = p->published;
			if (playback && !s->frozenAnchor.set)
				s->frozenAnchor = { true, playback->anchorPos, playback->anchorHeading };
		}
		p->committed = true;
		return true;
	}
	void SceneRuntime::FinishPreparation(const std::shared_ptr<PreparedTransition>& p, Outcome outcome, bool applied)
	{
		_preparations.erase(p->context.attempt);
		if (p->reset || outcome == Outcome::WorldReset)
			return;  // H2 reset already revoked edits; never mutate old actors
		bool live = false;
		{
			std::lock_guard l{ _lock };
			const auto*     s = Resolve(p->context.scene);
			live = s && s->preparation == p;
		}
		const bool success = live && outcome == Outcome::Committed && p->committed;
		if (!success && p->committed)
			StopGraph(p->cast, p->published);
		const auto restored = p->edit ? ActorState::Get().FinishEdit(p->edit, success) : ActorState::Status::Ok;
		if (!success)
			for (auto scope : p->newScopes) ActorState::Get().CloseScope(scope);
		std::erase_if(_resources, [&](const auto& e) {
			if (e.second.scene != p->context.scene)
				return false;
			return success ? !p->resources.contains(e.first) : e.second.attempt == p->context.attempt;
		});
		if (success)
			for (const auto& [key, provider] : p->resources) _resources[key] = { p->context.scene, provider, 0 };
		if (!live)
			return;
		{
			std::lock_guard l{ _lock };
			auto*           s = Resolve(p->context.scene);
			if (!s || s->preparation != p)
				return;
			s->preparation.reset();
			constexpr const char* names[] = { "committed", "vetoed", "failed", "cancelled", "timed-out", "owner-withdrawn", "world-reset" };
			s->preparationResult = names[static_cast<unsigned>(outcome)];
			s->preparationError = p->failureReason;
			if (success) {
				s->preparedScopes = p->scopes;
				s->transitioning = false;
				for (auto mechanism : { Mechanism::kEquipment, Mechanism::kHeldEquipment, Mechanism::kEquipItem })
					if (std::find(s->ledger.begin(), s->ledger.end(), mechanism) == s->ledger.end())
						s->ledger.push_back(mechanism);
			}
		}
		if (success) {
			Animation::GraphManager::GetSingleton().HoldForPreparation(p->cast.front(), p->published, false);
			if (p->context.kind == Kind::Start) {
				CompleteStart(p->context.scene, p->context.toScene, p->context.toNode, p->cast, p->playerInputLock, p->hideApparel, p->clearHeldItems, p->fade, p->overrides);
			} else if (p->context.kind != Kind::Stage) {
				if (p->context.kind == Kind::Handoff) {
					EngageDefaultPlayerInputLock(p->context.scene, p->playerInputLock, p->cast);
					if (!p->old.cameraOverridden)
						EngageDefaultCamera(p->context.scene, p->context.toNode, p->playerInputLock, {}, p->cast);
					EngageDefaultSceneControls(p->context.scene, p->context.toScene, std::nullopt, p->cast);
					Fire(p->context.scene, Event::kSceneHandoff, p->context.toNode, "", p->old.id);
				}
				SlotView view;
				if (LifecycleValid(p->context.scene, p->context.toNode, true) && SnapshotSlot(p->context.scene, view) && view.playbackId == p->published && view.node == p->context.toNode)
					Fire(p->context.scene, Event::kNodeEnter, p->context.toNode, "enter");
			}
			return;
		}
		REX::WARN("[Scene] preparation {} for scene {:#010x} ended with outcome {}", p->context.attempt, p->context.scene, static_cast<unsigned>(outcome));
		if (!p->failureReason.empty())
			REX::WARN("[Scene] preparation refusal: {}", p->failureReason);
		const bool resumed = MayResumePreparedSource(p->context.kind, p->automatic, p->cancelled,
			applied, p->exitFired, restored == ActorState::Status::Ok) &&
			Animation::GraphManager::GetSingleton().HoldForPreparation(p->old.participants.front(), p->old.playbackId, false);
		// Preflight refusal preserves a still-valid old motion. Once application or exit effects
		// have begun, interrupt conservatively; arbitrary provider side effects cannot be proven restored.
		if (p->context.kind == Kind::Start) {
			{
				std::lock_guard l{ _lock };
				if (auto* s = Resolve(p->context.scene))
					s->terminationCause = p->cancelled && !p->interrupted ? TerminationCause::kCancelled : TerminationCause::kInterrupted;
			}
			// The caller already owns a reserved handle. Give it a terminal acknowledgement
			// even though no SCENE_BEGIN/NODE_ENTER was published.
			Fire(p->context.scene, Event::kSceneEnd, p->context.toNode, "");
			ReleaseSlot(p->context.scene);
		} else if (!resumed) {
			{
				std::lock_guard l{ _lock };
				if (auto* s = Resolve(p->context.scene)) {
					s->id = p->old.id;
					s->node = p->old.node;
					s->participants = p->old.participants;
					s->definition = p->old.definition;
					s->terminationCause = p->cancelled && !p->interrupted ? TerminationCause::kCancelled : TerminationCause::kInterrupted;
				}
			}
			StopGraph(p->old.participants, p->old.playbackId);
			Fire(p->context.scene, Event::kSceneEnd, p->old.node, "");
			ReleaseSlot(p->context.scene);
		} else {
			EndTransition(p->context.scene);
		}
	}
	bool SceneRuntime::OnPlaybackStage(Animation::PlaybackId playback, std::int32_t stage, bool automatic)
	{
		auto p = std::make_shared<PreparedTransition>();
		{
			std::lock_guard l{ _lock };
			auto*           s = FindSlotForPlayback(playback, &p->context.scene);
			if (!s || s->participants.empty())
				return false;
			if (s->transitioning) {
				// The active transition consumes this automatic request. If it refuses, the
				// completed source must terminate instead of waiting forever on stageQueued.
				if (automatic && s->preparation)
					s->preparation->automatic = true;
				return automatic;
			}
			p->old = { s->id, s->node, s->participants, s->playbackId, s->definition, s->cameraOverridden };
			p->cast = s->participants;
			p->target = s->definition;
		}
		const auto current = Animation::GraphManager::GetSingleton().GetScenePlayback(p->cast.front(), playback);
		if (!current || stage < 0 || stage >= current->stageCount)
			return false;
		p->context.kind = Kind::Stage;
		p->context.fromScene = p->context.toScene = p->old.id;
		p->context.fromNode = p->context.toNode = p->old.node;
		p->context.fromStage = current->stage;
		p->context.toStage = stage;
		p->automatic = automatic;
		return QueuePreparation(p);
	}
	void SceneRuntime::ResetPreparation()
	{
		std::vector<std::shared_ptr<PreparedTransition>> pending;
		{
			std::lock_guard l{ _lock };
			for (auto& s : _slots)
				if (s.preparation) {
					s.preparation->reset = true;
					pending.push_back(s.preparation);
				}
		}
		_resources.clear();
		_preparationSaving = false;
		Util::NativeMainThreadQueue::Post([this, pending] {
			for (const auto& p : pending)
				if (p->context.attempt)
					_preparers.Cancel(p->context.attempt, Outcome::WorldReset);
		});
	}
	void SceneRuntime::ReleaseResources(std::int32_t scene)
	{
		std::erase_if(_resources, [&](const auto& e) { return e.second.scene == scene; });
	}
}
