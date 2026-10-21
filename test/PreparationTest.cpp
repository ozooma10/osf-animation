#include "Check.h"
#include "Preparation/Coordinator.h"
#include "Scene/TransitionCover.h"
#include <stdexcept>
using namespace OSF::Preparation;

int main()
{
    {
        OSF::Scene::TransitionCover cover{.started=1000};
        CHECK(!cover.PreparationReady(1299));
        CHECK(cover.PreparationReady(1300));
        CHECK(!cover.CanReveal(true));
        cover.Committed();
        CHECK(!cover.CanReveal(false)); // elapsed time alone never proves a stamped pose
        CHECK(cover.CanReveal(true)); // clip time need not advance
        CHECK(!cover.Expired(5999) && cover.Expired(6000));
        cover.Revealing(5500);
        CHECK(!cover.Finished(5999) && cover.Finished(6000));
        OSF::Scene::TransitionCover delayed{.started=1000};
        CHECK(!delayed.PreparationReady(6000)); // stalled frame cannot start a late handoff
    }
    for (auto kind : {Kind::Start, Kind::Handoff, Kind::Node, Kind::Stage}) {
        Coordinator c; std::vector<int> calls; int polls=0; Outcome result=Outcome::Failed;
        auto provider=c.Register({0,5000,
            [&](const Context& ctx, Phase phase) {
                CHECK(ctx.kind==kind && ctx.actors==std::vector<std::uint32_t>{14});
                calls.push_back(static_cast<int>(phase));
                if (phase==Phase::Ready && ++polls==1) return Decision::Pending;
                return Decision::Approve;
            }, [&](const Context&, Outcome o) { result=o; calls.push_back(8); }});
        CHECK(provider);
        Context ctx; ctx.scene=1; ctx.kind=kind; ctx.actors={14}; ctx.toNode="next";
        Hooks h{[]{return true;}, [&]{calls.push_back(4);return true;}, [&]{calls.push_back(5);return true;},
            []{return Decision::Approve;}, [&]{calls.push_back(6);return true;}, [&](Outcome o,bool applied){CHECK(applied && o==result);calls.push_back(9);}};
        auto a=c.Begin(ctx,h,100); CHECK(a); CHECK(!c.Begin(ctx,h,100));
        ctx.toNode="changed"; c.Tick(101);
        CHECK(c.Contains(a) && calls==std::vector<int>({0,4,5,1,2}));
        c.Tick(102);
        CHECK(!c.Contains(a) && result==Outcome::Committed && calls==std::vector<int>({0,4,5,1,2,2,3,6,8,9}));
    }
    for (auto outcome : {Outcome::Vetoed,Outcome::TimedOut,Outcome::OwnerWithdrawn,Outcome::Cancelled,Outcome::WorldReset}) {
        Coordinator c; int writes=0, finishes=0; Outcome got=Outcome::Committed;
        auto provider=c.Register({0,10,[&](const Context&,Phase) { return outcome==Outcome::Vetoed ? Decision::Veto : Decision::Pending; },
            [&](const Context&,Outcome o){++finishes;got=o;}});
        Context ctx;ctx.scene=2;ctx.actors={1};
        auto a=c.Begin(ctx,{[]{return true;},[]{return true;},[&]{++writes;return true;},[]{return Decision::Approve;},
            [&]{++writes;return true;},[&](Outcome o,bool applied){CHECK(!applied);got=o;}},0);
        c.Tick(1);
        if(outcome==Outcome::TimedOut)c.Tick(10);
        if(outcome==Outcome::OwnerWithdrawn)CHECK(c.Unregister(provider));
        if(outcome==Outcome::Cancelled)CHECK(c.Cancel(a));
        if(outcome==Outcome::WorldReset)c.Reset();
        CHECK(!c.Contains(a) && !writes && finishes==1 && got==outcome);
    }
    {
        Coordinator c;Token a=0;int committed=0,finished=0;
        c.Register({0,100,[&](const Context&,Phase p) { if(p==Phase::Apply)c.Cancel(a);return Decision::Approve; },
            [&](const Context&,Outcome o){CHECK(o==Outcome::Cancelled);++finished;}});
        Context ctx;ctx.scene=1;ctx.actors={1};
        a=c.Begin(ctx,{[]{return true;},[]{return true;},[]{return true;},[]{return Decision::Approve;},
            [&]{++committed;return true;},[&](Outcome,bool applied){CHECK(applied);++finished;}},0);
        c.Tick(1);CHECK(!committed && finished==2 && !c.Contains(a));
    }
    {
        Coordinator c;bool live=true;int commit=0;
        c.Register({0,100,[&](const Context&,Phase p){if(p==Phase::Validate)live=false;return Decision::Approve;},
            [](const Context&,Outcome o){CHECK(o==Outcome::Cancelled);}});
        Context ctx;ctx.scene=1;ctx.actors={1};
        c.Begin(ctx,{[&]{return live;},[]{return true;},[]{return true;},[]{return Decision::Approve;},
            [&]{++commit;return true;},[](Outcome,bool){}},0);
        c.Tick(1);CHECK(!commit && c.PendingCount()==0);
    }
    for (bool throws : {false,true}) {
        Coordinator c; int finish=0; bool applied=false;
        Context ctx;ctx.scene=1;ctx.actors={1};
        c.Begin(ctx,{[]{return true;},[]{return true;},[]{return true;},[]{return Decision::Approve;},
            [&]() -> bool {if(throws)throw std::runtime_error("playback failure");return false;},
            [&](Outcome o,bool mutated){CHECK(o==Outcome::Failed);applied=mutated;++finish;}},0);
        c.Tick(1);CHECK(finish==1 && applied && c.PendingCount()==0);
    }
    {
        Coordinator c;std::vector<int> order;
        for (int priority : {2,1,1})
            c.Register({priority,100,[&,priority](const Context&,Phase p){if(p==Phase::Plan)order.push_back(priority);return Decision::Approve;},
                [](const Context&,Outcome){}});
        Context ctx;ctx.scene=1;ctx.actors={1};
        c.Begin(ctx,{[]{return true;},[]{return true;},[]{return true;},[]{return Decision::Approve;},[]{return true;},[](Outcome,bool){}},0);
        c.Tick(1);CHECK(order==std::vector<int>({1,1,2}));
    }

	// A reset in one callback changes all later finalizers to WorldReset; no commit follows.
	for (auto resetPhase : { Phase::Plan, Phase::Apply, Phase::Ready, Phase::Validate }) {
		Coordinator c;
		bool        live = true;
		int         commit = 0;
		Outcome     result = Outcome::Failed;
		c.Register({ 0, 100,
			[&](const Context& ctx, Phase phase) {
				CHECK(c.CurrentProvider(ctx.attempt, phase).has_value());
				CHECK(c.Providers(ctx.attempt).size() == 1);
				if (phase == resetPhase)
					live = false;
				return Decision::Approve;
			},
			[&](const Context&, Outcome outcome) { CHECK(outcome == Outcome::WorldReset); } });
		Context ctx;
		ctx.scene = 1;
		ctx.actors = { 1 };
		Hooks h{ [&] { return live; }, [] { return true; }, [] { return true; }, [] { return Decision::Approve; },
			[&] {
				++commit;
				return true;
			},
			[&](Outcome o, bool) { result = o; }, [] { return Outcome::WorldReset; } };
		auto  id = c.Begin(ctx, h, 0);
		c.Tick(1);
		CHECK(result == Outcome::WorldReset && !commit && !c.CurrentProvider(id) && c.Providers(id).empty());
	}
	{
		Coordinator          c;
		bool                 live = true;
		std::vector<Outcome> results;
		c.Register({ 0, 100, [](const Context&, Phase) { return Decision::Approve; }, [&](const Context&, Outcome o) { results.push_back(o); } });
		c.Register({ 1, 100, [](const Context&, Phase) { return Decision::Approve; },
			[&](const Context&, Outcome o) {
				results.push_back(o);
				live = false;
			} });
		Context ctx;
		ctx.scene = 1;
		ctx.actors = { 1 };
		c.Begin(ctx, { [&] { return live; }, [] { return true; }, [] { return true; }, [] { return Decision::Approve; }, [] { return true; }, [&](Outcome o, bool) { results.push_back(o); }, [] { return Outcome::WorldReset; } }, 0);
		c.Tick(1);
		CHECK(results == std::vector<Outcome>({ Outcome::Committed, Outcome::WorldReset, Outcome::WorldReset }));
	}
	{
		Coordinator c;
		Token       provider = 0;
		int         commits = 0, finishes = 0;
		provider = c.Register({ 0, 100,
			[&](const Context&, Phase p) {
				if (p == Phase::Ready)
					CHECK(c.Unregister(provider));
				return Decision::Approve;
			},
			[&](const Context&, Outcome o) {
				CHECK(o == Outcome::OwnerWithdrawn);
				++finishes;
			} });
		Context ctx;
		ctx.scene = 1;
		ctx.actors = { 1 };
		c.Begin(ctx,
			{ [] { return true; }, [] { return true; }, [] { return true; }, [] { return Decision::Approve; },
				[&] {
					++commits;
					return true;
				},
				[](Outcome o, bool applied) { CHECK(o == Outcome::OwnerWithdrawn && applied); } },
			0);
		c.Tick(1);
		CHECK(!commits && finishes == 1 && !c.PendingCount());
	}
	{
		Coordinator c;
		int         commits = 0;
		Context     first;
		first.scene = 1;
		first.actors = { 1 };
		auto second = first;
		second.scene = 2;
		Hooks h{ [] { return true; }, [] { return true; }, [] { return true; }, [] { return Decision::Approve; },
			[&] {
				++commits;
				return true;
			},
			[](Outcome, bool) {} };
		auto  outer = h;
		outer.finish = [&](Outcome, bool) {
			CHECK(c.Begin(second, h, 1));
			c.Tick(1);
			CHECK(commits == 1);
		};
		c.Begin(first, outer, 0);
		c.Tick(1);
		CHECK(commits == 1 && c.PendingCount() == 1);
		c.Tick(2);
		CHECK(commits == 2 && !c.PendingCount());
		CHECK(!c.Register({ 0, 5001, [](const Context&, Phase) { return Decision::Approve; }, [](const Context&, Outcome) {} }));
	}
	return OSF::Test::Finish("preparation");
}
