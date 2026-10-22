#include "ActorState/Store.h"
#include "Profiling/Profile.h"

#include <algorithm>
#include <limits>

namespace OSF::ActorState
{
	namespace {
		bool Positive(Operation op) { return op == Operation::Equip || op == Operation::Preserve; }
		bool Overlap(const Item& a, const Item& b) { return a.object != b.object && !a.skin && !b.skin && (a.slots & b.slots) != 0; }
		Item Find(const std::vector<Item>& items, Object object, Item fallback = {}) {
			const auto it = std::find_if(items.begin(), items.end(), [&](const auto& item) { return item.object == object; });
			if (it != items.end()) return *it;
			fallback.object = object; fallback.count = 0; fallback.worn = false;
			return fallback;
		}
		void Merge(Status& result, Status next) { if (next != Status::Ok) result = next; }
	}

	Revision Store::ReadRevision(Actor actor) const
	{
		const auto it = _revisions.find(actor);
		return it == _revisions.end() ? Revision{_world} : it->second;
	}
	void Store::Changed(Actor actor, RevisionCause cause)
	{
		if (_preview) return;
		if (actor) {
			auto& revision = _revisions[actor]; revision.world = _world;
			if (cause == RevisionCause::Ownership) ++revision.ownership;
			else ++revision.equipment;
		}
		_notifications.push_back({++_sequence, actor, cause, ReadRevision(actor)});
		if (_notifications.size() > kNotificationLimit) _notifications.pop_front();
	}
	Notifications Store::Since(std::uint64_t cursor) const
	{
		Notifications out{_sequence, _world, cursor > _sequence ||
			(!_notifications.empty() && cursor < _notifications.front().sequence - 1)};
		if (!out.resnapshot) for (const auto& event : _notifications)
			if (event.sequence > cursor) out.events.push_back(event);
		return out;
	}
	std::optional<std::vector<Item>> Store::ReadInventory(Actor actor)
	{
		OSF_PROFILE_SCOPE("ActorState.ReadInventory");
		auto inventory = _backend.Read(actor);
		if (!_preview) {
			auto canonical = inventory;
			if (canonical) {
				std::erase_if(*canonical, [](const Item& i) { return !i.count; });
				std::sort(canonical->begin(), canonical->end());
			}
			const auto it = _observations.find(actor);
			if (it == _observations.end()) _observations.emplace(actor, std::move(canonical));
			else if (it->second != canonical) {
				it->second = std::move(canonical);
				Changed(actor, RevisionCause::Equipment);
			}
		}
		return inventory;
	}
	std::optional<EquipmentObservation> Store::InspectEquipment(Actor actor, Object object)
	{
		if (!actor || !object) return {};
		const auto description = _backend.Describe(object);
		if (!description) return {};
		const auto inventory = ReadInventory(actor);
		if (!inventory) return {};
		EquipmentObservation out{Find(*inventory, object, *description)};
		for (const auto& item : *inventory)
			if (item.worn && Overlap(out.item, item)) out.conflictingWornSlots |= out.item.slots & item.slots;
		return out;
	}

	Handle Store::Next()
	{
		if (!_next || _next == std::numeric_limits<Handle>::max()) return 0;
		return _next++;
	}
	Handle Store::CreateScope(std::string_view owner, Lifetime lifetime, std::uint64_t session)
	{
		if (owner.empty() || owner.size() > 256 || (lifetime != Lifetime::Persistent && lifetime != Lifetime::Session) ||
			(lifetime == Lifetime::Session) != (session != 0)) return 0;
		const auto id = Next();
		if (id) _scopes.emplace(id, Scope{std::string(owner), lifetime, session});
		return id;
	}
	bool Store::Active(const Claim& claim) const
	{
		const auto scope = _scopes.find(claim.scope);
		return scope != _scopes.end() && !(_saving && scope->second.lifetime == Lifetime::Session);
	}
	bool Store::Owns(Handle scope, Actor actor, Object object, std::uint32_t group) const
	{
		return std::any_of(_claims.begin(), _claims.end(), [&](const auto& entry) {
			const auto& c = entry.second;
			return c.scope == scope && c.request.actor == actor && c.request.object == object && c.request.group == group;
		});
	}
	std::vector<Object> Store::Kept(Actor actor) const
	{
		std::vector<Object> out;
		for (const auto& [id, c] : _claims)
			if (Active(c) && c.request.actor == actor && Positive(c.request.operation)) out.push_back(c.request.object);
		return out;
	}
	std::vector<Object> Store::Hidden(Actor actor) const
	{
		std::vector<Object> out;
		for (const auto& [id, c] : _claims)
			if (Active(c) && c.request.actor == actor && !Positive(c.request.operation)) out.push_back(c.request.object);
		return out;
	}

	Result Store::Acquire(Handle scope, const Request& request, bool replaceGroup,
		std::optional<std::uint32_t> replaceFallbackGroup)
	{
		OSF_PROFILE_SCOPE("ActorState.Acquire");
		if (!HasScope(scope) || !request.actor || !request.object || request.operation > Operation::HideOptional ||
			(request.allowAdd && request.operation != Operation::Equip)) return {};
		if (_saving && _scopes.at(scope).lifetime == Lifetime::Session) return {Status::Unavailable};
		const auto described = _backend.Describe(request.object);
		const auto inventory = ReadInventory(request.actor);
		if (!described || !inventory) return {Status::Unavailable};
		const auto item = Find(*inventory, request.object, *described);
		if (item.skin && !Positive(request.operation)) return {Status::Conflict};
		if (!item.count && !(request.operation == Operation::Equip && request.allowAdd)) return {Status::Missing};

		std::vector<std::pair<Handle, Claim>> replaced;
		for (const auto& [id, c] : _claims) {
			if (c.request.actor != request.actor) continue;
			if (c.scope == scope && c.request.object == request.object && c.request.group == request.group &&
				c.request.operation == request.operation && c.request.allowAdd == request.allowAdd)
				return {_preview ? Status::Ok : Apply(request.actor), id};
			const auto other = _backend.Describe(c.request.object);
			if (!other) return {Status::Unavailable};
			const bool positive = Positive(request.operation), otherPositive = Positive(c.request.operation);
			const bool conflict = (positive && otherPositive && Overlap(item, *other)) ||
				(request.object == c.request.object && ((positive && c.request.operation == Operation::Hide) ||
					(otherPositive && request.operation == Operation::Hide)));
			if (!conflict) continue;
			// Replacement may retire only this owner's equip lanes. The scene adapter explicitly
			// lets authored equipment supersede its own optional gear lane; public callers cannot.
			const bool replaceLane = (replaceGroup && c.request.group == request.group) ||
				(replaceFallbackGroup && c.request.group == *replaceFallbackGroup);
			if (replaceLane && c.scope == scope &&
				request.operation == Operation::Equip && c.request.operation == Operation::Equip) replaced.emplace_back(id, c);
			else return {Status::Conflict};
		}
		const auto id = Next();
		if (!id) return {Status::Unavailable};
		if (!_preview) _records[request.actor].try_emplace(item.object, Record{item, item.worn});
		for (const auto& [old, c] : replaced) _claims.erase(old);
		_claims.emplace(id, Claim{scope, request}); // ownership precedes all engine mutation
		Changed(request.actor, RevisionCause::Ownership);
		const auto status = _preview ? Status::Ok : Apply(request.actor, Displacement::Any);
		if (status != Status::Ok) {
			_claims.erase(id);
			for (const auto& [old, c] : replaced) _claims.emplace(old, c);
			Changed(request.actor, RevisionCause::Ownership);
			Apply(request.actor); // retained records allow a later Reconcile to retry failed cleanup
			return {status};
		}
		return {Status::Ok, id};
	}

	void Store::Observe(std::map<Object, Record>& records, const std::vector<Item>& inventory)
	{
		for (auto& [object, r] : records) {
			const auto now = Find(inventory, object, r.expected);
			auto intended = r.expected;
			switch (r.pending) {
			case Pending::Worn: intended.worn = true; break;
			case Pending::Unworn: intended.worn = false; break;
			case Pending::Add: ++intended.count; break;
			case Pending::Remove: if (intended.count) --intended.count; intended.worn = false; break;
			case Pending::None: break;
			}
			const bool completed = now.count == intended.count && now.worn == intended.worn;
			const bool unchanged = now.count == r.expected.count && now.worn == r.expected.worn;
			if (completed && r.pending == Pending::Add) r.added = true;
			if (completed && r.pending == Pending::Remove) r.added = false;
			if (!completed && !unchanged) {
				++_externalChanges;
				// A pending backend operation survives a temporarily unavailable readback. Only a
				// different observation rebases ownership; an unchanged read is a failed operation.
				if (now.count != r.expected.count) r.added = false;
				r.baselineWorn = now.worn; r.changed = false;
			}
			r.pending = Pending::None;
			r.expected = now;
		}
	}

	Status Store::Apply(Actor actor, Displacement displace)
	{
		OSF_PROFILE_SCOPE("ActorState.Apply");
		auto read = ReadInventory(actor);
		if (!read) return Status::Unavailable;
		auto& records = _records[actor];
		Observe(records, *read);
		struct Demand { bool worn{}, strictHide{}, allowAdd{}; };
		std::map<Object, Demand> demands;
		for (const auto& [id, c] : _claims) {
			if (c.request.actor != actor || !Active(c)) continue;
			auto& d = demands[c.request.object];
			d.worn |= Positive(c.request.operation);
			d.strictHide |= c.request.operation == Operation::Hide;
			d.allowAdd |= c.request.allowAdd;
		}
		for (const auto& [object, d] : demands)
			if (d.worn && d.strictHide) return Status::Conflict;

		// Refresh expected observations after our own writes, never treating them as external edits.
		auto refresh = [&]() {
			auto next = ReadInventory(actor);
			if (!next) return false;
			read = std::move(next);
			Observe(records, *read);
			return true;
		};
		auto wear = [&](Object object, bool worn) {
			const auto item = Find(*read, object, records.at(object).expected);
			if (!item.count) return !worn;
			if (item.worn == worn) return true;
			records.at(object).changed = true;
			records.at(object).pending = worn ? Pending::Worn : Pending::Unworn;
			Changed(actor, RevisionCause::Equipment); // attempts invalidate even when readback is lost
			const bool submitted = _backend.SetWorn(actor, object, worn);
			const bool observed = refresh();
			return submitted && observed && Find(*read, object).worn == worn;
		};
		auto blocked = [&](Object object) {
			const auto item = records.at(object).expected;
			return std::any_of(read->begin(), read->end(), [&](const Item& other) { return other.worn && Overlap(item, other); });
		};

		// Drop no-longer-required owned equip/hidden state before applying surviving positive claims.
		for (auto& [object, r] : records) {
			const auto d = demands.find(object);
			const bool positive = d != demands.end() && d->second.worn;
			if (positive) continue;
			const bool hide = d != demands.end();
			if ((hide || (r.changed && !r.baselineWorn) || r.added) && !wear(object, false)) return Status::Failed;
			if (r.added) {
				// We add one only when absent. A different observed count relinquishes this right above.
				if (r.expected.count == 1) {
					r.pending = Pending::Remove;
					Changed(actor, RevisionCause::Equipment);
					const bool removed = _backend.Remove(actor, object);
					const bool observed = refresh();
					if (!removed || !observed || records.at(object).expected.count) return Status::Failed;
				}
				r.added = false;
			}
		}
		for (const auto& [object, d] : demands) {
			if (!d.worn) continue;
			auto& r = records.at(object);
			if (!r.expected.count) {
				if (!d.allowAdd) return Status::Missing;
				r.pending = Pending::Add;
				Changed(actor, RevisionCause::Equipment);
				const bool added = _backend.Add(actor, object);
				const bool observed = refresh();
				if (!added || !observed || r.expected.count != 1) return Status::Failed;
			}
			const auto snapshot = *read;
			for (const auto& other : snapshot) {
				if (!other.worn || !Overlap(r.expected, other)) continue;
				const auto captured = records.find(other.object);
				const bool restoredBaseline = captured != records.end() && captured->second.changed && captured->second.baselineWorn;
				if (displace == Displacement::None ||
					(displace == Displacement::CapturedBaseline && !restoredBaseline) ||
					(demands.contains(other.object) && demands.at(other.object).worn)) return Status::Conflict;
				records.try_emplace(other.object, Record{other, other.worn});
				if (!wear(other.object, false)) return Status::Failed;
			}
			if (!wear(object, true)) return Status::Failed;
		}
		// Restore only still-owned deltas. Never equip a removed item or displace new live equipment.
		for (auto& [object, r] : records) {
			if (demands.contains(object) || !r.changed || !r.baselineWorn || !r.expected.count) continue;
			if (blocked(object)) {
				// Keep a displaced baseline while our own positive demand occupies its slot.
				// An unrelated newly worn item instead relinquishes this restoration obligation.
				const bool ownedBlocker = std::any_of(read->begin(), read->end(), [&](const Item& other) {
					return other.worn && Overlap(r.expected, other) && demands.contains(other.object) && demands.at(other.object).worn;
				});
				if (!ownedBlocker) { r.baselineWorn = r.expected.worn; r.changed = false; }
				continue;
			}
			if (!wear(object, true)) return Status::Failed;
		}
		const bool claimed = std::any_of(_claims.begin(), _claims.end(), [&](const auto& entry) { return entry.second.request.actor == actor; });
		// A prepared transition can temporarily release the actor's last claim. Keep its
		// provenance until the edit closes, so rollback can restore the source requirements.
		const bool editing = std::any_of(_edits.begin(), _edits.end(), [&](const auto& edit) { return std::any_of(edit.second.claims.begin(), edit.second.claims.end(), [&](const auto& claim) { return claim.second.request.actor == actor; }); });
		if (!claimed && !editing)
			_records.erase(actor);
		return Status::Ok;
	}
	Status Store::Reconcile(Actor actor) { return _records.contains(actor) ? Apply(actor) : Status::Ok; }
	Status Store::ReconcileAll(const std::vector<Actor>& actors)
	{
		Status result = Status::Ok;
		for (auto actor : actors) Merge(result, Reconcile(actor));
		return result;
	}
	Status Store::Release(Handle scope, Handle claim)
	{
		const auto it = _claims.find(claim);
		if (!HasScope(scope) || it == _claims.end() || it->second.scope != scope) return Status::Invalid;
		const auto actor = it->second.request.actor;
		_claims.erase(it);
		Changed(actor, RevisionCause::Ownership);
		return _preview ? Status::Ok : Apply(actor);
	}
	Status Store::ReleaseGroup(Handle scope, std::uint32_t group)
	{
		if (!HasScope(scope)) return Status::Invalid;
		std::vector<Actor> actors;
		std::erase_if(_claims, [&](const auto& entry) {
			if (entry.second.scope != scope || entry.second.request.group != group) return false;
			actors.push_back(entry.second.request.actor); return true;
		});
		std::sort(actors.begin(), actors.end());
		actors.erase(std::unique(actors.begin(), actors.end()), actors.end());
		for (auto actor : actors) Changed(actor, RevisionCause::Ownership);
		return _preview ? Status::Ok : ReconcileAll(actors);
	}
	Status Store::CloseScope(Handle scope)
	{
		if (!HasScope(scope)) return Status::Invalid;
		std::vector<Actor> actors;
		std::erase_if(_claims, [&](const auto& entry) {
			if (entry.second.scope != scope) return false;
			actors.push_back(entry.second.request.actor); return true;
		});
		_scopes.erase(scope);
		std::sort(actors.begin(), actors.end());
		actors.erase(std::unique(actors.begin(), actors.end()), actors.end());
		for (auto actor : actors) Changed(actor, RevisionCause::Ownership);
		return _preview ? Status::Ok : ReconcileAll(actors);
	}
	Status Store::CloseSession(std::uint64_t session)
	{
		if (!session) return Status::Invalid;
		std::vector<Handle> scopes;
		for (const auto& [id, s] : _scopes)
			if (s.lifetime == Lifetime::Session && s.session == session) scopes.push_back(id);
		Status result = Status::Ok;
		for (const auto id : scopes) Merge(result, CloseScope(id));
		return result;
	}
	Status Store::SetSaveWindow(bool saving)
	{
		OSF_PROFILE_SCOPE("ActorState.SetSaveWindow");
		if (_saving == saving) return Status::Ok;
		_saving = saving;
		std::vector<Actor> suspended;
		for (const auto& [id, claim] : _claims)
			if (_scopes.at(claim.scope).lifetime == Lifetime::Session) suspended.push_back(claim.request.actor);
		std::sort(suspended.begin(), suspended.end());
		suspended.erase(std::unique(suspended.begin(), suspended.end()), suspended.end());
		for (auto actor : suspended) Changed(actor, RevisionCause::Ownership);
		std::vector<Actor> actors;
		for (const auto& [actor, records] : _records) actors.push_back(actor);
		Status result = Status::Ok;
		for (auto actor : actors) Merge(result, Apply(actor, saving ? Displacement::None : Displacement::CapturedBaseline));
		return result;
	}
    Status Store::PreviewPlan(const std::vector<Change>& changes)
    {
        OSF_PROFILE_SCOPE("ActorState.PreviewPlan");
        // Admission needs ownership and token/save state, not reconciliation or observation history.
        Store preview(_backend);
        preview._preview = true;
        preview._next = _next;
        preview._saving = _saving;
        preview._scopes = _scopes;
        preview._claims = _claims;
        return preview.ApplyPlan(changes);
    }
    Status Store::ApplyPlan(const std::vector<Change>& changes)
    {
        OSF_PROFILE_SCOPE("ActorState.ApplyPlan");
        for (const auto& c : changes) {
            const auto result = c.releaseGroup ? ReleaseGroup(c.scope, c.request.group) :
                Acquire(c.scope, c.request, c.replaceGroup, c.replaceFallbackGroup).status;
            if (result != Status::Ok && !c.optional) return result;
        }
        return Status::Ok;
    }
    Handle Store::BeginEdit(const std::vector<Handle>& scopes)
    {
        if (scopes.empty() || std::any_of(scopes.begin(), scopes.end(), [&](Handle scope) {
            return !HasScope(scope) || std::any_of(_edits.begin(), _edits.end(), [&](const auto& e) {
                return std::find(e.second.scopes.begin(), e.second.scopes.end(), scope) != e.second.scopes.end();
            });
        })) return 0;
        const auto id = Next();
        if (!id) return 0;
        Edit edit; edit.scopes = scopes;
        for (const auto& [claim, c] : _claims)
            if (std::find(scopes.begin(), scopes.end(), c.scope) != scopes.end()) edit.claims.emplace(claim, c);
        _edits.emplace(id, std::move(edit));
        return id;
    }
    Status Store::FinishEdit(Handle id, bool commit)
    {
        const auto it = _edits.find(id);
        if (it == _edits.end()) return Status::Invalid;
        auto edit = std::move(it->second); _edits.erase(it);
		if (commit) {
			// Only this edit can have retained already-restored records. Failed cleanup,
			// including another actor's pending cleanup, must remain retryable.
			for (const auto& [claim, c] : edit.claims) {
				const auto actor = c.request.actor;
				const auto record = _records.find(actor);
				if (record == _records.end()) continue;
				const bool restored = std::all_of(record->second.begin(), record->second.end(), [](const auto& entry) {
					const auto& r = entry.second;
					return r.pending == Pending::None && !r.added &&
						(!r.changed || !r.expected.count || r.expected.worn == r.baselineWorn);
				});
				if (restored &&
					std::none_of(_claims.begin(), _claims.end(), [&](const auto& current) { return current.second.request.actor == actor; }) &&
					std::none_of(_edits.begin(), _edits.end(), [&](const auto& current) { return std::any_of(current.second.claims.begin(), current.second.claims.end(), [&](const auto& saved) { return saved.second.request.actor == actor; }); }))
					_records.erase(record);
			}
			return Status::Ok;
		}
		std::vector<Actor> actors;
        std::erase_if(_claims, [&](const auto& e) {
            if (std::find(edit.scopes.begin(), edit.scopes.end(), e.second.scope) == edit.scopes.end()) return false;
            if (!edit.claims.contains(e.first)) Changed(e.second.request.actor, RevisionCause::Ownership);
            actors.push_back(e.second.request.actor); return !edit.claims.contains(e.first);
        });
        for (const auto& [claim, c] : edit.claims) {
            if (HasScope(c.scope) && _claims.emplace(claim, c).second) Changed(c.request.actor, RevisionCause::Ownership);
            actors.push_back(c.request.actor);
        }
        std::sort(actors.begin(), actors.end());
        actors.erase(std::unique(actors.begin(), actors.end()), actors.end());
        Status result = Status::Ok;
        for (auto actor : actors) Merge(result, Apply(actor, Displacement::CapturedBaseline));
        return result;
    }
	void Store::Reset()
	{
		_claims.clear(); _scopes.clear(); _records.clear(); _edits.clear(); _saving = false;
		_revisions.clear(); _observations.clear(); ++_world; Changed(0, RevisionCause::Reset);
		// Never reset _next: old-world tokens remain invalid even if form/session IDs recur.
	}
}
