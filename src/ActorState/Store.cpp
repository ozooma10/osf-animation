#include "ActorState/Store.h"

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

	Result Store::Acquire(Handle scope, const Request& request, bool replaceGroup)
	{
		if (!HasScope(scope) || !request.actor || !request.object || request.operation > Operation::HideOptional ||
			(request.allowAdd && request.operation != Operation::Equip)) return {};
		if (_saving && _scopes.at(scope).lifetime == Lifetime::Session) return {Status::Unavailable};
		const auto described = _backend.Describe(request.object);
		const auto inventory = _backend.Read(request.actor);
		if (!described || !inventory) return {Status::Unavailable};
		const auto item = Find(*inventory, request.object, *described);
		if (item.skin && !Positive(request.operation)) return {Status::Conflict};
		if (!item.count && !(request.operation == Operation::Equip && request.allowAdd)) return {Status::Missing};

		std::vector<std::pair<Handle, Claim>> replaced;
		for (const auto& [id, c] : _claims) {
			if (c.request.actor != request.actor) continue;
			if (c.scope == scope && c.request.object == request.object && c.request.group == request.group &&
				c.request.operation == request.operation && c.request.allowAdd == request.allowAdd)
				return {Apply(request.actor, false), id};
			const auto other = _backend.Describe(c.request.object);
			if (!other) return {Status::Unavailable};
			const bool positive = Positive(request.operation), otherPositive = Positive(c.request.operation);
			const bool conflict = (positive && otherPositive && Overlap(item, *other)) ||
				(request.object == c.request.object && ((positive && c.request.operation == Operation::Hide) ||
					(otherPositive && request.operation == Operation::Hide)));
			if (!conflict) continue;
			// Authored sequential replacements may retire only their own equip lane.
			if (replaceGroup && c.scope == scope && c.request.group == request.group &&
				request.operation == Operation::Equip && c.request.operation == Operation::Equip) replaced.emplace_back(id, c);
			else return {Status::Conflict};
		}
		const auto id = Next();
		if (!id) return {Status::Unavailable};
		_records[request.actor].try_emplace(item.object, Record{item, item.worn});
		for (const auto& [old, c] : replaced) _claims.erase(old);
		_claims.emplace(id, Claim{scope, request}); // ownership precedes all engine mutation
		const auto status = Apply(request.actor, true);
		if (status != Status::Ok) {
			_claims.erase(id);
			for (const auto& [old, c] : replaced) _claims.emplace(old, c);
			Apply(request.actor, false); // retained records allow a later Reconcile to retry failed cleanup
			return {status};
		}
		return {Status::Ok, id};
	}

	Status Store::Apply(Actor actor, bool displace)
	{
		auto read = _backend.Read(actor);
		if (!read) return Status::Unavailable;
		auto& records = _records[actor];
		for (auto& [object, r] : records) {
			const auto now = Find(*read, object, r.expected);
			if (now.count != r.expected.count || now.worn != r.expected.worn) {
				++_externalChanges;
				// No exact instance provenance: relinquish destruction if counts changed externally.
				if (now.count != r.expected.count) r.added = false;
				r.baselineWorn = now.worn;
				r.changed = false;
			}
			r.expected = now;
		}
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
			auto next = _backend.Read(actor);
			if (!next) return false;
			read = std::move(next);
			for (auto& [object, r] : records) r.expected = Find(*read, object, r.expected);
			return true;
		};
		auto wear = [&](Object object, bool worn) {
			const auto item = Find(*read, object, records.at(object).expected);
			if (!item.count) return !worn;
			if (item.worn == worn) return true;
			records.at(object).changed = true;
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
				const bool added = _backend.Add(actor, object);
				const bool observed = refresh();
				if (observed && r.expected.count) r.added = true;
				if (!added || !observed || r.expected.count != 1) return Status::Failed;
			}
			const auto snapshot = *read;
			for (const auto& other : snapshot) {
				if (!other.worn || !Overlap(r.expected, other)) continue;
				if (!displace || (demands.contains(other.object) && demands.at(other.object).worn)) return Status::Conflict;
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
		if (!claimed) _records.erase(actor);
		return Status::Ok;
	}
	Status Store::Reconcile(Actor actor) { return _records.contains(actor) ? Apply(actor, false) : Status::Ok; }
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
		return Apply(actor, false);
	}
	Status Store::ReleaseGroup(Handle scope, std::uint32_t group)
	{
		if (!HasScope(scope)) return Status::Invalid;
		std::vector<Actor> actors;
		std::erase_if(_claims, [&](const auto& entry) {
			if (entry.second.scope != scope || entry.second.request.group != group) return false;
			actors.push_back(entry.second.request.actor); return true;
		});
		return ReconcileAll(actors);
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
		return ReconcileAll(actors);
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
		if (_saving == saving) return Status::Ok;
		_saving = saving;
		std::vector<Actor> actors;
		for (const auto& [actor, records] : _records) actors.push_back(actor);
		Status result = Status::Ok;
		for (auto actor : actors) Merge(result, Apply(actor, !saving));
		return result;
	}
	void Store::Reset()
	{
		_claims.clear(); _scopes.clear(); _records.clear(); _saving = false;
		// Never reset _next: old-world tokens remain invalid even if form/session IDs recur.
	}
}
