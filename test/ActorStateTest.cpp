#include "Check.h"
#include "ActorState/Store.h"

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
	return OSF::Test::Finish("actor-state");
}
