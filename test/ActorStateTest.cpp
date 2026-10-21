#include "ActorState/Store.h"
#include "Check.h"
#include "Preparation/Coordinator.h"

#include <algorithm>
#include <map>
using namespace OSF::ActorState;

struct Fake final : Backend {
	std::map<Actor, std::vector<Item>> actors;
	std::map<Object, Item> forms{{1,{1,0,1}},{2,{2,0,1}},{3,{3,0,2}},{4,{4,0,4}}};
	int writes{}, failWrite{-1};
	bool unavailable{};
	int loseReadAfterWrite{-1};
	std::optional<std::vector<Item>> Read(Actor a) override { if (unavailable) return {}; return actors[a]; }
	std::optional<Item> Describe(Object o) override { if (!forms.contains(o)) return {}; return forms[o]; }
	Item& Get(Actor a, Object o) {
		auto& items = actors[a];
		auto it = std::find_if(items.begin(),items.end(),[&](auto& i){return i.object==o;});
		if (it == items.end()) { items.push_back(forms.at(o)); return items.back(); }
		return *it;
	}
	bool SetWorn(Actor a,Object o,bool worn) override {
		++writes; if (writes == loseReadAfterWrite) unavailable = true; if (writes == failWrite) return false;
		Get(a,o).worn = worn; return true;
	}
	bool Add(Actor a,Object o) override { ++writes; if (writes == loseReadAfterWrite) unavailable = true; if (writes == failWrite) return false; ++Get(a,o).count; return true; }
	bool Remove(Actor a,Object o) override { ++writes; if (writes == loseReadAfterWrite) unavailable = true; if (writes == failWrite) return false; --Get(a,o).count; return true; }
	void Seed(Actor a,Object o,bool worn=true) { Get(a,o).count=1; Get(a,o).worn=worn; }
};
Request Equip(Object o, std::uint32_t group=1) { return {10,o,Operation::Equip,group,true}; }
Request Hide(Object o, std::uint32_t group=2) { return {10,o,Operation::HideOptional,group,false}; }
Request Preserve(Object o) { return {10,o,Operation::Preserve,3,false}; }

int main()
{
	{
		Fake f;
		auto state = ReadEquipment(f,10,1);
		CHECK(state && state->item.slots == 1 && !state->item.count && !state->item.worn);
		f.Seed(10,1); f.Seed(10,2); f.Seed(10,3);
		state = ReadEquipment(f,10,1);
		CHECK(state && state->item.worn && state->conflictingWornSlots == 1);
		f.Get(10,2).skin = true;
		CHECK(ReadEquipment(f,10,1)->conflictingWornSlots == 0);
		f.Get(10,2).skin = false; f.Get(10,2).worn = false;
		CHECK(ReadEquipment(f,10,1)->conflictingWornSlots == 0);
		CHECK(f.writes == 0);
		f.unavailable = true;
		CHECK(!ReadEquipment(f,10,1));
		CHECK(!ReadEquipment(f,10,999) && !ReadEquipment(f,0,1));
	}
	{
		Fake f; f.Seed(10,1); Store s(f);
		auto scene=s.CreateScope("scene",Lifetime::Session,42), device=s.CreateScope("device",Lifetime::Persistent);
		auto body=s.Acquire(scene,Equip(2)); CHECK(body);
		CHECK(!f.Get(10,1).worn && f.Get(10,2).worn);
		auto belt=s.Acquire(device,Equip(3)); CHECK(belt);
		CHECK(s.CloseSession(42)==Status::Ok);
		CHECK(f.Get(10,1).worn && !f.Get(10,2).count && f.Get(10,3).worn);
		CHECK(s.HasScope(device) && !s.HasScope(scene));
		CHECK(s.CloseScope(device)==Status::Ok && !f.Get(10,3).count);
		CHECK(s.PendingActorCount()==0);
	}
	{
		Fake f; Store s(f);
		auto a=s.CreateScope("a",Lifetime::Session,1), b=s.CreateScope("b",Lifetime::Persistent);
		auto x=s.Acquire(a,Equip(2)), y=s.Acquire(b,Equip(2)); CHECK(x && y);
		CHECK(f.Get(10,2).count==1);
		CHECK(s.Release(a,y.handle)==Status::Invalid);
		CHECK(s.CloseScope(a)==Status::Ok && f.Get(10,2).worn);
		CHECK(s.Release(b,y.handle)==Status::Ok && !f.Get(10,2).count);
		CHECK(s.Release(b,y.handle)==Status::Invalid);
	}
	for (bool worn : {false,true}) {
		Fake f; f.Seed(10,2,worn); Store s(f);
		auto a=s.CreateScope("a",Lifetime::Session,1);
		CHECK(s.Acquire(a,Equip(2)));
		CHECK(s.CloseScope(a)==Status::Ok);
		CHECK(f.Get(10,2).count==1 && f.Get(10,2).worn==worn);
	}
	{
		Fake f; f.Seed(10,1); Store s(f);
		auto a=s.CreateScope("a",Lifetime::Session,1), b=s.CreateScope("b",Lifetime::Persistent);
		CHECK(s.Acquire(a,Hide(1)));
		CHECK(s.Acquire(b,Preserve(1)));
		CHECK(f.Get(10,1).worn);
		CHECK(s.Acquire(a,{10,1,Operation::Hide,4}).status==Status::Conflict);
		CHECK(s.ReleaseGroup(a,2)==Status::Ok && f.Get(10,1).worn);
		CHECK(s.CloseScope(a)==Status::Ok && f.Get(10,1).worn);
		CHECK(s.CloseScope(b)==Status::Ok && f.Get(10,1).worn);
	}
	{
		Fake f; f.Seed(10,1); Store s(f);
		auto a=s.CreateScope("a",Lifetime::Session,1), b=s.CreateScope("b",Lifetime::Session,2);
		CHECK(s.Acquire(a,Hide(1))); CHECK(s.Acquire(b,Hide(1)));
		CHECK(s.CloseScope(a)==Status::Ok && !f.Get(10,1).worn);
		CHECK(s.CloseScope(b)==Status::Ok && f.Get(10,1).worn);
	}
	{
		Fake f; f.Seed(10,1); Store s(f);
		auto a=s.CreateScope("a",Lifetime::Session,1), b=s.CreateScope("b",Lifetime::Persistent);
		CHECK(s.Acquire(a,Equip(2)));
		auto before=f.writes;
		CHECK(s.Acquire(b,Equip(1)).status==Status::Conflict && f.writes==before);
		CHECK(s.Acquire(a,Equip(1),true));
		CHECK(!f.Get(10,2).count && f.Get(10,1).worn);
		CHECK(s.CloseScope(a)==Status::Ok && f.Get(10,1).worn);
	}
	{
		Fake f; Store s(f);
		auto a=s.CreateScope("a",Lifetime::Session,1);
		auto x=s.Acquire(a,Equip(2));
		auto before=f.writes;
		auto again=s.Acquire(a,Equip(2)); CHECK(again.handle==x.handle && f.writes==before && s.ClaimCount()==1);
		CHECK(s.Acquire(a,Equip(3,9)));
		CHECK(s.ReleaseGroup(a,1)==Status::Ok && !f.Get(10,2).count && f.Get(10,3).worn);
		CHECK(s.CloseScope(a)==Status::Ok && !f.Get(10,3).count);
	}
	{
		Fake f; f.Seed(10,1); Store s(f);
		auto a=s.CreateScope("a",Lifetime::Session,1), b=s.CreateScope("b",Lifetime::Persistent);
		CHECK(s.Acquire(a,Hide(1))); CHECK(s.Acquire(a,Equip(2))); CHECK(s.Acquire(b,Equip(3)));
		CHECK(s.SetSaveWindow(true)==Status::Ok);
		CHECK(f.Get(10,1).worn && !f.Get(10,2).count && f.Get(10,3).worn);
		CHECK(s.Acquire(a,Equip(4)).status==Status::Unavailable);
		CHECK(s.SetSaveWindow(false)==Status::Ok);
		CHECK(!f.Get(10,1).worn && f.Get(10,2).worn && f.Get(10,3).worn);
		CHECK(s.SetSaveWindow(true)==Status::Ok);
		CHECK(s.CloseScope(a)==Status::Ok);
		CHECK(s.SetSaveWindow(false)==Status::Ok && !f.Get(10,2).count && f.Get(10,1).worn);
	}
	{
		Fake f; f.Seed(10,1); Store s(f);
		auto a=s.CreateScope("a",Lifetime::Session,1);
		CHECK(s.Acquire(a,Hide(1)));
		f.Seed(10,2); // unregistered device equipped while the original apparel is hidden
		CHECK(s.CloseScope(a)==Status::Ok);
		CHECK(f.Get(10,2).worn && !f.Get(10,1).worn);
	}
	{
		Fake f; Store s(f);
		auto a=s.CreateScope("a",Lifetime::Session,1);
		CHECK(s.Acquire(a,Equip(2)));
		f.Get(10,2).count=2; // ambiguous third-party inventory mutation: don't destroy its copy
		CHECK(s.CloseScope(a)==Status::Ok && f.Get(10,2).count==2);
		CHECK(s.ExternalChanges()==1);
	}
	for (int fail : {1,2,3}) {
		Fake f; f.Seed(10,1); f.failWrite=fail; Store s(f);
		auto a=s.CreateScope("a",Lifetime::Session,1);
		CHECK(!s.Acquire(a,Equip(2)));
		CHECK(s.ClaimCount()==0);
		CHECK(s.CloseScope(a)==Status::Ok);
		CHECK(f.Get(10,1).worn && !f.Get(10,2).count);
	}
	{
		Fake f; Store s(f);
		auto a=s.CreateScope("a",Lifetime::Session,1); auto x=s.Acquire(a,Equip(2)); CHECK(x);
		f.unavailable=true;
		CHECK(s.CloseScope(a)==Status::Unavailable && s.PendingActorCount()==1);
		f.unavailable=false;
		CHECK(s.Reconcile(10)==Status::Ok && !f.Get(10,2).count);
	}
	{
		Fake f; Store s(f);
		auto a=s.CreateScope("a",Lifetime::Persistent); auto x=s.Acquire(a,Equip(2)); CHECK(x);
		auto before=f.writes;
		s.Reset();
		auto b=s.CreateScope("b",Lifetime::Persistent);
		CHECK(b!=a && s.Release(b,x.handle)==Status::Invalid);
		CHECK(s.ClaimCount()==0 && s.PendingActorCount()==0 && f.writes==before);
		CHECK(!s.CreateScope("",Lifetime::Persistent));
		CHECK(!s.CreateScope("bad",Lifetime::Session));
		CHECK(!s.CreateScope("bad",Lifetime::Persistent,1));
		CHECK(s.Acquire(b,{10,4,Operation::Preserve}).status==Status::Missing);
	}
	for (int lost : {1, 2, 3}) {
		Fake f; f.Seed(10,1); f.loseReadAfterWrite=lost; Store s(f);
		auto scope=s.CreateScope("readback",Lifetime::Session,1);
		CHECK(!s.Acquire(scope,Equip(2)));
		CHECK(s.PendingActorCount()==1);
		f.unavailable=false;
		CHECK(s.Reconcile(10)==Status::Ok);
		CHECK(f.Get(10,1).worn && !f.Get(10,2).count);
		CHECK(s.ExternalChanges()==0);
	}
	{
		Fake f; f.Seed(10,1); Store s(f);
		auto scope=s.CreateScope("save",Lifetime::Session,1);
		CHECK(s.Acquire(scope,Equip(2)));
		CHECK(s.SetSaveWindow(true)==Status::Ok && f.Get(10,1).worn);
		CHECK(s.SetSaveWindow(false)==Status::Ok && f.Get(10,2).worn && !f.Get(10,1).worn);
		CHECK(s.SetSaveWindow(true)==Status::Ok);
		f.Get(10,1).worn=false;
		f.forms[4].slots=1; f.Seed(10,4); // a new device takes the slot during the save window
		CHECK(s.SetSaveWindow(false)==Status::Conflict);
		CHECK(f.Get(10,4).worn && !f.Get(10,2).worn);
		CHECK(s.CloseScope(scope)==Status::Ok);
		CHECK(f.Get(10,4).worn && !f.Get(10,2).count && !f.Get(10,1).worn);
	}
	for (int lost : {1,2}) {
		Fake f; Store s(f);
		auto scope=s.CreateScope("release-readback",Lifetime::Session,1);
		CHECK(s.Acquire(scope,Equip(2)));
		f.loseReadAfterWrite=f.writes+lost;
		CHECK(s.CloseScope(scope)==Status::Failed);
		f.unavailable=false;
		CHECK(s.Reconcile(10)==Status::Ok && !f.Get(10,2).count);
		CHECK(s.PendingActorCount()==0 && s.ExternalChanges()==0);
	}
	{
		Fake f; f.Seed(10,1); Store s(f);
		auto scope=s.CreateScope("scene",Lifetime::Session,1), device=s.CreateScope("device",Lifetime::Persistent);
		auto gear=s.Acquire(scope,{10,1,Operation::Equip,4}); CHECK(gear);
		auto protectedGear=s.Acquire(device,Preserve(1)); CHECK(protectedGear);
		CHECK(s.Acquire(scope,Equip(2),true,4).status==Status::Conflict);
		CHECK(s.Owns(scope,10,1,4) && f.Get(10,1).worn);
		CHECK(s.CloseScope(device)==Status::Ok);
		// Failed authored replacement restores the optional gear claim.
		f.failWrite=f.writes+1;
		CHECK(!s.Acquire(scope,Equip(2),true,4));
		CHECK(s.Owns(scope,10,1,4) && f.Get(10,1).worn);
		CHECK(s.Acquire(scope,Equip(2),true,4));
		CHECK(!s.Owns(scope,10,1,4) && !f.Get(10,1).worn && f.Get(10,2).worn);
		CHECK(s.Acquire(scope,{10,3,Operation::Equip,4,true}));
		CHECK(s.ReleaseGroup(scope,1)==Status::Ok && f.Get(10,3).worn && f.Get(10,1).worn);
		CHECK(s.CloseScope(scope)==Status::Ok && !f.Get(10,3).count);
	}
    {
        Fake f;f.Seed(10,1);Store s(f);
        auto scene=s.CreateScope("scene",Lifetime::Session,1), device=s.CreateScope("device",Lifetime::Persistent);
        CHECK(s.Acquire(scene,Equip(2)));
        std::vector<Store::Change> plan{{scene,Equip(1),false,true,false,{}}};
        const auto writes=f.writes;
        CHECK(s.PreviewPlan(plan)==Status::Ok && f.writes==writes && f.Get(10,2).worn);
        auto edit=s.BeginEdit({scene});CHECK(edit && !s.BeginEdit({scene}));
        CHECK(s.ApplyPlan(plan)==Status::Ok && f.Get(10,1).worn);
        CHECK(s.Acquire(device,Equip(3)));
        CHECK(s.FinishEdit(edit,false)==Status::Ok && f.Get(10,2).worn && f.Get(10,3).worn);
        CHECK(s.CloseScope(scene)==Status::Ok && f.Get(10,1).worn && f.Get(10,3).worn);
        CHECK(s.FinishEdit(edit,false)==Status::Invalid);
        CHECK(s.CloseScope(device)==Status::Ok);
    }

	for (auto outcome : { OSF::Preparation::Outcome::Committed, OSF::Preparation::Outcome::TimedOut, OSF::Preparation::Outcome::WorldReset, OSF::Preparation::Outcome::Failed }) {
		using namespace OSF::Preparation;
		Fake f;
		f.Seed(10, 1);
		Store       store(f);
		Coordinator coordinator;
		auto        scene = store.CreateScope("scene", Lifetime::Session, 9);
		auto        device = store.CreateScope("device", Lifetime::Persistent);
		CHECK(store.Acquire(scene, Equip(2)));
		std::vector<Store::Change> changes{ { scene, { 0, 0, Operation::Equip, 1 }, true, false, false, {} },  // authored exit unequip
			{ scene, Equip(1), false, false, false, {} } };                                                    // required destination item
		Handle                     edit = 0;
		bool                       ready = false, world = true;
		int                        commits = 0;
		Outcome                    got = Outcome::Vetoed;
		coordinator.Register({ 0, 100,
			[&](const Context&, Phase phase) {
				if (phase == Phase::Ready)
					return ready ? Decision::Approve : Decision::Pending;
				return Decision::Approve;
			},
			[](const Context&, Outcome) {} });
		Context ctx;
		ctx.scene = 9;
		ctx.actors = { 10 };
		coordinator.Begin(ctx,
			{ [&] { return world; },
				[&] {
					auto writes = f.writes;
					auto status = store.PreviewPlan(changes);
					CHECK(writes == f.writes);
					return status == Status::Ok;
				},
				[&] {
					edit = store.BeginEdit({ scene });
					return edit && store.ApplyPlan(changes) == Status::Ok;
				},
				[] { return Decision::Approve; },
				[&] {
					++commits;
					return outcome != Outcome::Failed;
				},
				[&](Outcome o, bool applied) {
					got = o;
					CHECK(applied);
					if (o != Outcome::WorldReset)
						CHECK(store.FinishEdit(edit, o == Outcome::Committed) == Status::Ok);
				},
				[] { return Outcome::WorldReset; } },
			0);
		coordinator.Tick(1);
		CHECK(commits == 0 && coordinator.PendingCount() == 1 && f.Get(10, 1).worn);
		CHECK(store.Acquire(device, Equip(3)));  // unrelated persistent change while waiting
		if (outcome == Outcome::WorldReset) {
			auto writes = f.writes;
			world = false;
			store.Reset();
			coordinator.Tick(2);
			CHECK(f.writes == writes && store.ClaimCount() == 0);
		} else {
			ready = outcome != Outcome::TimedOut;
			coordinator.Tick(ready ? 2 : 100);
			CHECK(f.Get(10, 3).worn && f.Get(10, 3).count == 1);
			CHECK(f.Get(10, outcome == Outcome::Committed ? 1 : 2).worn);
		}
		CHECK(got == outcome && !coordinator.PendingCount());
		CHECK(commits == ((outcome == Outcome::Committed || outcome == Outcome::Failed) ? 1 : 0));
	}
	{
		Fake  f;
		Store store(f);
		auto  scene = store.CreateScope("scene", Lifetime::Session, 1), device = store.CreateScope("device", Lifetime::Persistent);
		CHECK(store.Acquire(device, Equip(1)));
		auto writes = f.writes;
		CHECK(store.PreviewPlan({ { scene, Equip(2), false, false, false, {} } }) == Status::Conflict);
		CHECK(f.writes == writes && f.Get(10, 1).worn);  // refusal never displaces a persistent owner
	}
	{
		Fake f;
		f.Seed(10, 1);
		Store store(f);
		auto  scene = store.CreateScope("release", Lifetime::Session, 1);
		CHECK(store.Acquire(scene, Equip(2)));
		auto edit = store.BeginEdit({ scene });
		CHECK(edit);
		CHECK(store.ReleaseGroup(scene, 1) == Status::Ok && store.PendingActorCount() == 1);
		CHECK(store.FinishEdit(edit, true) == Status::Ok && store.PendingActorCount() == 0);
		CHECK(f.Get(10, 1).worn && !f.Get(10, 2).count);
	}
	for (bool sameActor : { false, true }) {
		Fake f;
		f.Seed(10, 1);
		Store store(f);
		const auto owner = store.CreateScope("cleanup", Lifetime::Persistent);
		CHECK(store.Acquire(owner, Equip(2)));
		const auto unrelated = store.CreateScope("unrelated", Lifetime::Session, 2);
		const auto edit = store.BeginEdit({ sameActor ? owner : unrelated });
		CHECK(edit);
		f.failWrite = f.writes + 1;
		CHECK(store.CloseScope(owner) == Status::Failed && store.PendingActorCount() == 1);
		const auto writes = f.writes;
		CHECK(store.FinishEdit(edit, true) == Status::Ok && store.PendingActorCount() == 1);
		CHECK(f.writes == writes); // committing does not mutate equipment after validation
		CHECK(store.Reconcile(10) == Status::Ok && store.PendingActorCount() == 0);
		CHECK(f.Get(10, 1).worn && !f.Get(10, 2).count);
	}
	{
		Fake f;
		f.Seed(10, 1);
		Store store(f);
		const auto owner = store.CreateScope("readback-cleanup", Lifetime::Persistent);
		CHECK(store.Acquire(owner, Equip(2)));
		const auto edit = store.BeginEdit({ owner });
		f.loseReadAfterWrite = f.writes + 1;
		CHECK(store.CloseScope(owner) == Status::Failed);
		CHECK(store.FinishEdit(edit, true) == Status::Ok && store.PendingActorCount() == 1);
		f.unavailable = false;
		CHECK(store.Reconcile(10) == Status::Ok && store.PendingActorCount() == 0);
		CHECK(f.Get(10, 1).worn && !f.Get(10, 2).count);
	}
	// Revision delivery is independent of consumers and engine events.
	{
		Fake f; f.Seed(10,1); Store s(f);
		const auto initial=s.ReadRevision(10);
		CHECK(initial.world && !initial.ownership && !initial.equipment && s.Since(0).events.empty());
		auto a=s.CreateScope("one",Lifetime::Persistent), b=s.CreateScope("two",Lifetime::Persistent);
		CHECK(s.ReadRevision(10)==initial);
		CHECK(s.InspectEquipment(10,1));
		auto observed=s.ReadRevision(10); auto cursor=s.Since(0).cursor;
		CHECK(!observed.equipment && !observed.ownership && f.writes==0);
		auto x=s.Acquire(a,Preserve(1)); CHECK(x);
		auto owned=s.ReadRevision(10);
		CHECK(owned.ownership==1 && owned.equipment==observed.equipment);
		auto changes=s.Since(cursor);
		CHECK(changes.events.size()==1 && changes.events[0].actor==10 && changes.events[0].cause==RevisionCause::Ownership && changes.events[0].revision==owned);
		cursor=changes.cursor;
		CHECK(s.Acquire(a,Preserve(1)).handle==x.handle && s.ReadRevision(10)==owned);
		CHECK(s.Release(b,x.handle)==Status::Invalid && s.ReleaseGroup(a,99)==Status::Ok && s.Since(cursor).events.empty());
		CHECK(s.Acquire(b,Preserve(1)) && s.ReadRevision(10).ownership==2 && f.writes==0);
		CHECK(s.CloseScope(a)==Status::Ok && s.ReadRevision(10).ownership==3 && f.Get(10,1).worn);
		CHECK(s.ReadRevision(20)==initial);
		f.Seed(20,3); CHECK(s.Acquire(b,{20,3,Operation::Preserve}));
		CHECK(s.ReadRevision(10).ownership==3 && s.ReadRevision(20).ownership==1);
		f.unavailable=true; cursor=s.Since(0).cursor;
		CHECK(s.CloseScope(b)==Status::Unavailable && s.ReadRevision(10).ownership==4 && s.ReadRevision(20).ownership==2);
		changes=s.Since(cursor);
		CHECK(std::any_of(changes.events.begin(),changes.events.end(),[](auto& e){return e.actor==10 && e.cause==RevisionCause::Ownership;}));
		f.unavailable=false; CHECK(s.Reconcile(10)==Status::Ok);
	}
	{
		Fake f; f.Seed(10,1); f.Seed(10,3); Store s(f);
		CHECK(s.InspectEquipment(10,1)); auto revision=s.ReadRevision(10);
		std::reverse(f.actors[10].begin(),f.actors[10].end());
		CHECK(s.InspectEquipment(10,1) && s.ReadRevision(10)==revision);
		f.Get(10,3).worn=false; // includes other forms in this actor's observed inventory
		CHECK(s.InspectEquipment(10,1) && s.ReadRevision(10).equipment>revision.equipment && f.writes==0);
		revision=s.ReadRevision(10); f.unavailable=true;
		CHECK(!s.InspectEquipment(10,1) && s.ReadRevision(10).equipment>revision.equipment);
		revision=s.ReadRevision(10);
		CHECK(!s.InspectEquipment(10,1) && s.ReadRevision(10)==revision);
		f.unavailable=false;
		CHECK(s.InspectEquipment(10,1) && s.ReadRevision(10).equipment>revision.equipment);
	}
	{
		Fake f; f.Seed(10,1); Store s(f);
		auto scene=s.CreateScope("session",Lifetime::Session,1), persistent=s.CreateScope("device",Lifetime::Persistent);
		CHECK(s.Acquire(scene,Preserve(1)) && s.Acquire(persistent,Preserve(1)));
		auto revision=s.ReadRevision(10); auto cursor=s.Since(0).cursor;
		CHECK(s.PreviewPlan({{scene,Equip(2),false,true}})==Status::Conflict);
		CHECK(s.PreviewPlan({{scene,Equip(3),false,true}})==Status::Ok);
		CHECK(s.ReadRevision(10)==revision && s.Since(cursor).events.empty() && f.writes==0);
		CHECK(s.SetSaveWindow(true)==Status::Ok && s.ReadRevision(10).ownership==revision.ownership+1 && f.writes==0);
		revision=s.ReadRevision(10);
		CHECK(s.SetSaveWindow(true)==Status::Ok && s.ReadRevision(10)==revision);
		CHECK(s.SetSaveWindow(false)==Status::Ok && s.ReadRevision(10).ownership==revision.ownership+1);
		auto edit=s.BeginEdit({scene}); revision=s.ReadRevision(10);
		CHECK(s.FinishEdit(edit,false)==Status::Ok && s.ReadRevision(10)==revision);
		edit=s.BeginEdit({scene}); CHECK(s.Acquire(scene,Equip(3))); revision=s.ReadRevision(10);
		CHECK(s.FinishEdit(edit,false)==Status::Ok && s.ReadRevision(10).ownership>revision.ownership && s.ReadRevision(10).equipment>revision.equipment);
		CHECK(s.HasScope(persistent) && f.Get(10,1).worn);
		CHECK(s.CloseSession(1)==Status::Ok && s.HasScope(persistent));
		revision=s.ReadRevision(10);
		CHECK(s.SetSaveWindow(true)==Status::Ok && s.SetSaveWindow(false)==Status::Ok && s.ReadRevision(10)==revision);
		const auto writes=f.writes; cursor=s.Since(0).cursor; s.Reset();
		CHECK(s.ReadRevision(10).world!=revision.world && !s.ReadRevision(10).ownership && !s.ReadRevision(10).equipment && f.writes==writes);
		auto events=s.Since(cursor);
		CHECK(events.events.size()==1 && !events.events[0].actor && events.events[0].cause==RevisionCause::Reset && events.world==s.ReadRevision(10).world);
	}
	for (int lost : {1,2,3}) {
		Fake f; f.Seed(10,1); Store s(f); CHECK(s.InspectEquipment(10,1));
		auto revision=s.ReadRevision(10); auto scope=s.CreateScope("failed",Lifetime::Persistent);
		f.loseReadAfterWrite=lost;
		CHECK(!s.Acquire(scope,Equip(2)) && !s.ClaimCount());
		CHECK(s.ReadRevision(10).ownership>revision.ownership && s.ReadRevision(10).equipment>revision.equipment);
		f.unavailable=false; revision=s.ReadRevision(10);
		CHECK(s.Reconcile(10)==Status::Ok && s.ReadRevision(10).equipment>revision.equipment);
	}
	{
		Fake f; f.Seed(10,1); Store s(f); CHECK(s.InspectEquipment(10,1));
		const auto cursor=s.Since(0).cursor;
		for (std::size_t i=0;i<kNotificationLimit+1;++i) { f.Get(10,1).worn=!f.Get(10,1).worn; CHECK(s.InspectEquipment(10,1)); }
		const auto latest=s.Since(cursor);
		CHECK(latest.resnapshot && latest.events.empty());
		CHECK(s.Since(latest.cursor+1).resnapshot && s.Since(latest.cursor).events.empty());
		CHECK(!s.Since(latest.cursor-kNotificationLimit).resnapshot && s.Since(latest.cursor-kNotificationLimit).events.size()==kNotificationLimit);
	}
	return OSF::Test::Finish("actor-state");
}
