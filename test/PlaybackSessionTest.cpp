// Exercise the production shared-clock transport without actors, hooks or a running game.
#include "Check.h"
#include "Animation/Scene.h"
#include "Scene/SceneFoundation.h"
#include "Scene/AlignmentState.h"

#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <tuple>
#include <utility>

namespace
{
	using namespace OSF::Animation;
	using OSF::Test::Check;
	using Stage = PlaybackSession::SegmentData;
	using Hits = std::vector<std::pair<std::uint8_t, std::string>>;

	Stage Segment(float a_duration, int a_loops = 0, float a_timer = 0)
	{
		Stage stage;
		stage.duration = a_duration;
		stage.loops = a_loops;
		stage.timer = a_timer;
		return stage;
	}
	Hits Drain(PlaybackSession& a_session)
	{
		std::vector<FiredMark> fired;
		a_session.DrainFiredMarks(fired);
		Hits hits;
		for (const auto& mark : fired) hits.emplace_back(mark.lane, mark.token);
		return hits;
	}
	void Near(float a_value, float a_expected, std::string_view a_label)
	{
		Check(std::isfinite(a_value) && std::abs(a_value - a_expected) < 0.0001f, a_label);
	}
	void Run(std::string_view a_name, const std::function<void()>& a_test)
	{
		std::cout << "playback session: " << a_name << '\n';
		try {
			a_test();
		} catch (const std::exception& error) {
			Check(false, std::string(a_name) + " threw: " + error.what());
		}
	}
}

int main()
{
	Run("alignment persistence and working-copy ownership", [] {
		using namespace OSF::Scene::Alignment;
		auto table = Parse(nlohmann::json::parse(R"({"formatVersion":1,"scenes":{"demo":{"#s2":{"partner":{"y":5,"heading":90},"lead":{}}}}})"));
		Check(table.has_value(), "valid delta table parses");
		if (!table) return;
		Check(!table->at("demo").at("#s2").contains("lead"), "zero rows are pruned");
		Near(Find(*table, "demo", "#s2", "partner").heading, 90 * OSF::Util::kDegToRadF, "file degrees convert to runtime radians");
		Near(Find(*table, "demo", "#s1", "partner").y, 0, "no cross-node fallback");
		WorkingCopy work;
		work.Enter("#s2", { Find(*table, "demo", "#s2", "partner") })[0].y += 1;
		work.Enter("#s1", { {} });
		Near(work.Enter("#s2", { {} })[0].y, 6, "revisit retains unsaved delta instead of reseeding");
		Put(*table, "demo", "#s2", "partner", work.nodes.at("#s2")[0]);
		const auto roundtrip = Parse(Serialize(*table));
		Check(roundtrip.has_value(), "saved table parses");
		Near(Find(*roundtrip, "demo", "#s2", "partner").y, 6, "save replaces delta instead of adding twice");
		work.nodes.at("#s2")[0] = {};
		Near(Find(*table, "demo", "#s2", "partner").y, 6, "reset changes only the working copy until save");
		Put(*table, "demo", "#s2", "partner", {});
		Check(table->empty(), "saving reset prunes empty node and scene");
		Check(!Parse(nlohmann::json::parse(R"({"formatVersion":1,"scenes":{"demo":{"n":{"r":{"x":"bad"}}}}})")), "one invalid row rejects the whole document");
		Check(!Parse(nlohmann::json{ { "formatVersion", 2 }, { "scenes", nlohmann::json::object() } }), "unknown version refused");
	});
	Run("scene speed survives rebuilds and pause is per scene", [] {
		OSF::Scene::SceneSpeed a, b;
		a.Set(0.5f);
		PlaybackPlan plan;
		for (int node = 0; node < 5; ++node) {
			a.Apply(plan);
			Near(plan.speed, 0.5f, "plan receives persistent speed on every entry");
		}
		a.Set(a.ToggleValue());
		b.Set(2); b.Set(b.ToggleValue());
		a.Apply(plan); Near(plan.speed, 0, "paused scene remains paused across nodes");
		a.Set(a.ToggleValue()); b.Set(b.ToggleValue());
		Near(a.value, 0.5f, "scene A resumes its own speed");
		Near(b.value, 2, "scene B resumes its own speed");
		Check(!a.Set(std::numeric_limits<float>::quiet_NaN()) && a.value == 0.5f, "non-finite speed cannot corrupt state");
		a.Set(200); Near(a.value, 100, "API speed upper bound");
	});
	Run("frozen placement and actor-owned restore baseline", [] {
		auto* a = reinterpret_cast<RE::Actor*>(1);
		auto* b = reinterpret_cast<RE::Actor*>(2);
		OSF::Scene::ActorBaseline baseline{ { a, { { 10, 0, 0 }, 1 } }, { b, { { 20, 0, 0 }, 2 } } };
		PlaybackPlan plan;
		const OSF::Scene::WorldAnchor frozen{ true, { 100, 200, 300 }, 0.5f };
		for (int node = 0; node < 5; ++node) {
			OSF::Scene::ApplyScenePlacement(plan, {}, frozen, baseline, { a, b });
			Check(plan.anchorResolved && !plan.restoreTransformsOnEnd && plan.anchorPos.x == 100,
				"frozen anchor survives node rebuilds without enabling restoration");
		}
		OSF::Scene::ApplyScenePlacement(plan, frozen, {}, baseline, { b, a });
		Check(plan.restoreTransformsOnEnd && plan.baselineTransforms[0].first.x == 20 &&
			plan.baselineTransforms[1].first.x == 10, "role swap restores each actor to its own original transform");
	});
	Run("one-shot completion parks on final pose and retains final marks", [] {
		PlaybackSession session;
		auto stage = Segment(2, 1);
		stage.marks = { { .fraction = 0, .lane = 0, .token = "enter" },
			{ .fraction = 0.5f, .lane = 2, .token = "middle" },
			{ .atEnd = true, .lane = 3, .token = "exit" } };
		session.stages = { stage };
		Check(session.SetSegment(0), "initial segment selected");
		int owner = 0;
		const auto tick = session.Advance(&owner, 10);
		Check(session.ended && tick.holdEnd && tick.stage == 0 && tick.time == 2,
			"oversized delta completes one loop and parks at clip end");
		Check(session.endReason == PlaybackEndReason::kLoopComplete, "one-shot reports loop completion");
		Check(Drain(session) == Hits{ { 0, "enter" }, { 2, "middle" }, { 3, "exit" } },
			"all crossed marks including final mark are available before teardown");
		const auto stopped = session.Advance(&owner, 1);
		Check(stopped.time == 2 && stopped.holdEnd && Drain(session).empty(), "ended session stays parked without replaying marks");
	});
	Run("timer and loop targets choose the earliest completion", [] {
		for (const auto [timer, expectedTime, expectedReason] : std::array{
			std::tuple{ 0.5f, 0.5f, PlaybackEndReason::kTimerComplete },
			std::tuple{ 1.0f, 1.0f, PlaybackEndReason::kTimerComplete },
			std::tuple{ 2.0f, 1.0f, PlaybackEndReason::kLoopComplete } }) {
			PlaybackSession session;
			session.stages = { Segment(1, 1, timer) };
			Check(session.SetSegment(0), "timed segment selected");
			int owner = 0;
			const auto tick = session.Advance(&owner, 8);
			Check(session.ended && session.endReason == expectedReason, "earliest boundary determines completion; timer wins a tie");
			Near(tick.time, expectedTime, "completion position is clamped to first boundary");
			Check(tick.holdEnd == (expectedReason == PlaybackEndReason::kLoopComplete), "only loop completion requests end-pose hold");
		}
		PlaybackSession timed;
		timed.stages = { Segment(1, 0, 2.5f) };
		Check(timed.SetSegment(0), "looping timed segment selected");
		int owner = 0;
		const auto tick = timed.Advance(&owner, 8);
		Check(timed.ended && timed.endReason == PlaybackEndReason::kTimerComplete, "timer tracks elapsed time across clip wraps");
		Near(tick.time, 0.5f, "timer ends at the clip-local remainder after two wraps");
	});
	Run("segment changes reset time and consume the current update", [] {
		PlaybackSession session;
		auto first = Segment(1, 1);
		first.marks = { { .atEnd = true, .token = "first-end" } };
		auto second = Segment(2, 1);
		second.marks = { { .fraction = 0, .token = "second-start" } };
		session.stages = { first, second };
		Check(session.SetSegment(0), "multi-stage session initialized");
		int owner = 0;
		const auto transition = session.Advance(&owner, 100);
		Check(!session.ended && transition.stage == 1 && transition.time == 0 && session.CurrentSegment() == 1,
			"large update advances one segment and leaves new segment at its first pose");
		Check(Drain(session) == Hits{ { 0, "first-end" } }, "next-segment start mark waits for its first update");
		const auto next = session.Advance(&owner, 0.5f);
		Check(next.stage == 1 && next.time == 0.5f && Drain(session) == Hits{ { 0, "second-start" } },
			"next update advances new duration and fires its entrance mark");
		const auto end = session.Advance(&owner, 3);
		Check(session.ended && end.stage == 1 && end.time == 2 && end.holdEnd, "final segment completes at its own clip duration");
	});
	Run("whole-session loops and manual navigation rearm stage marks", [] {
		PlaybackSession session;
		auto stage = Segment(1, 1);
		stage.marks = { { .fraction = 0, .token = "once-per-stage" } };
		session.stages = { stage, Segment(2, 1) };
		session.loopWhole = true;
		Check(session.SetSegment(0), "looping session initialized");
		int owner = 0;
		(void)session.Advance(&owner, 1);
		Check(Drain(session) == Hits{ { 0, "once-per-stage" } }, "initial entrance fires");
		const auto wrap = session.Advance(&owner, 2);
		Check(!session.ended && wrap.stage == 0 && wrap.time == 0, "whole-session loop returns to segment zero without ending");
		(void)session.Advance(&owner, 0.25f);
		Check(Drain(session) == Hits{ { 0, "once-per-stage" } }, "whole-session restart rearms nonrepeating marks");
		Check(!session.SetSegment(-1) && !session.SetSegment(2), "invalid navigation rejected");
		Near(session.GetPlaybackSnapshot().time, 0.25f, "invalid navigation preserves the active clock");
		session.loopWhole = false;
		Check(session.SetSegment(1), "manual navigation selects final stage");
		(void)session.Advance(&owner, 3);
		session.endQueued = true;
		Check(session.SetSegment(0) && !session.ended && !session.endQueued, "manual navigation revives ended session and clears queued end");
		(void)session.Advance(&owner, 0.25f);
		Check(Drain(session) == Hits{ { 0, "once-per-stage" } }, "manual restart rearms entrance marks");
	});
	Run("marks use half-open intervals and drain once", [] {
		PlaybackSession session;
		auto stage = Segment(1);
		stage.marks = {
			{ .fraction = 0, .token = "zero" },
			{ .fraction = 0.5f, .lane = 0, .token = "fraction" },
			{ .seconds = 0.5f, .lane = 1, .token = "absolute" },
			{ .seconds = 1.0f, .everyLoop = true, .token = "at-duration" },
			{ .seconds = 2.0f, .everyLoop = true, .token = "past-duration" },
			{ .atEnd = true, .lane = 3, .token = "end" }
		};
		session.stages = { stage };
		Check(session.SetSegment(0), "mark-boundary session initialized");
		int owner = 0;
		(void)session.Advance(&owner, 0.5f);
		Check(Drain(session) == Hits{ { 0, "zero" } }, "mark at next-time boundary waits until the following interval");
		(void)session.Advance(&owner, 0.25f);
		Check(Drain(session) == Hits{ { 0, "fraction" }, { 1, "absolute" } }, "equal-time lanes preserve authored order and tokens");
		Check(Drain(session).empty(), "second drain cannot replay dispatched marks");
		(void)session.Advance(&owner, 0.25f);
		Check(Drain(session) == Hits{ { 3, "end" } }, "atEnd fires at wrap while absolute marks at or beyond duration do not");
		(void)session.Advance(&owner, 2);
		Check(Drain(session).empty(), "one-shot marks and atEnd do not repeat on later loops");
	});
	Run("repeat marks and loop accounting are independent of update subdivision", [] {
		auto stage = Segment(1);
		stage.marks = { { .fraction = 0, .everyLoop = true, .lane = 0, .token = "zero" },
			{ .fraction = 0.25f, .everyLoop = true, .lane = 1, .token = "quarter" },
			{ .fraction = 0.75f, .everyLoop = true, .lane = 2, .token = "late" },
			{ .atEnd = true, .lane = 3, .token = "first-end" } };
		auto collect = [&](unsigned count, float delta) {
			PlaybackSession session;
			session.stages = { stage };
			Check(session.SetSegment(0), "subdivision comparison initialized");
			int owner = 0;
			Hits hits;
			for (unsigned i = 0; i < count; ++i) {
				(void)session.Advance(&owner, delta);
				const auto batch = Drain(session);
				hits.insert(hits.end(), batch.begin(), batch.end());
			}
			Near(session.GetPlaybackSnapshot().time, 0, "18 exact clip loops finish at phase zero");
			Check(!session.ended, "unbounded looping remains active");
			return hits;
		};
		const auto one = collect(1, 18);
		const auto subdivided = collect(144, 0.125f);
		Check(one == subdivided && one.size() == 55, "large delta preserves all 54 repeated hits and one first-end hit in order");
	});
	Run("pause and invalid time/speed never advance or dispatch", [] {
		PlaybackSession session;
		auto stage = Segment(2);
		stage.marks = { { .fraction = 0, .token = "start" } };
		session.stages = { stage };
		Check(session.SetSegment(0), "invalid-time session initialized");
		int owner = 0;
		for (float delta : { 0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() }) {
			Near(session.Advance(&owner, delta).time, 0, "invalid delta leaves clock unchanged");
			Check(Drain(session).empty(), "invalid delta fires no marks");
		}
		for (float speed : { 0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() }) {
			session.speed = speed;
			Near(session.Advance(&owner, 0.5f).time, 0, "paused/invalid speed leaves clock unchanged");
			Check(Drain(session).empty(), "paused/invalid speed fires no marks");
		}
		session.speed = 2;
		Near(session.Advance(&owner, std::numeric_limits<float>::max()).time, 0, "overflowing scaled delta is rejected");
		Near(session.Advance(&owner, 0.25f).time, 0.5f, "valid speed resumes at scaled time");
		Check(Drain(session) == Hits{ { 0, "start" } }, "resuming dispatches the previously unconsumed start mark once");
	});
	Run("shared owner prevents participant double advances", [] {
		PlaybackSession session;
		session.stages = { Segment(2) };
		Check(session.SetSegment(0), "shared-owner session initialized");
		int owner = 0, follower = 0;
		Near(session.Advance(&owner, 0.25f).time, 0.25f, "first participant advances shared time");
		Near(session.Advance(&follower, 0.25f).time, 0.25f, "second participant observes the same shared time");
		session.speed = 2;
		Near(session.Advance(&owner, 0.25f).time, 0.75f, "shared speed applies on owner updates");
		const auto snapshot = session.GetPlaybackSnapshot();
		Check(snapshot.time == 0.75f && snapshot.duration == 2 && snapshot.speed == 2 && snapshot.stage == 0,
			"snapshot describes one coherent transport state");
	});
	Run("held poses fire reached marks once and use elapsed timer", [] {
		for (float hold : { 0.0f, 0.5f, 1.0f }) {
			PlaybackSession session;
			auto stage = Segment(2, 0, 1);
			stage.hold = hold;
			stage.marks = { { .fraction = 0, .everyLoop = true, .token = "start" },
				{ .fraction = 0.5f, .everyLoop = true, .token = "middle" },
				{ .atEnd = true, .token = "end" } };
			session.stages = { stage };
			Check(session.SetSegment(0), "held stage initialized");
			int owner = 0;
			const auto first = session.Advance(&owner, 0.25f);
			Check(!session.ended && first.time <= 2 * hold && (hold != 1 || first.time < 2),
				"hold is parked immediately, with end hold below wrap boundary");
			const auto reached = Drain(session);
			Check(reached.size() == (hold == 0 ? 1u : hold == 0.5f ? 2u : 3u), "only marks at or before held pose fire");
			session.speed = 0;
			(void)session.Advance(&owner, 10);
			Check(!session.ended && Drain(session).empty(), "paused hold does not expire its timer or replay marks");
			session.speed = 2;
			const auto last = session.Advance(&owner, 0.375f);
			Check(session.ended && session.endReason == PlaybackEndReason::kTimerComplete && !last.holdEnd,
				"hold timer uses speed-scaled elapsed time");
			Near(last.time, first.time, "held pose stays fixed until termination");
			Check(Drain(session).empty(), "repeating authored marks remain one-shot while frozen");
		}
	});
	Run("untimed hold and empty session remain stable", [] {
		PlaybackSession session;
		auto stage = Segment(4);
		stage.hold = 0.25f;
		session.stages = { stage };
		Check(session.SetSegment(0), "untimed hold initialized");
		int owner = 0;
		Near(session.Advance(&owner, 1000).time, 1, "untimed hold does not drift over a long update");
		Check(!session.ended, "untimed hold stays active for explicit navigation or stop");
		PlaybackSession empty;
		Check(!empty.SetSegment(0), "empty session has no valid segment");
		const auto tick = empty.Advance(&owner, 1);
		Check(tick.time == 0 && tick.stage == 0 && !empty.ended && Drain(empty).empty(), "empty session is safe to tick");
	});
	return OSF::Test::Finish("Playback session");
}
