#include "Animation/GraphManager.h"
#include "Util/NativeMainThreadQueue.h"

namespace OSF::Animation
{
	std::optional<GraphManager::AlignmentSnapshot> GraphManager::ReadAlignment(RE::Actor* a_actor, PlaybackId a_expectedPlayback)
	{
		// One coherent roster/segment snapshot; the animation hooks hold shared stateLock.
		std::unique_lock lock{stateLock};
		const auto found = graphs.find(a_actor);
		if (found == graphs.end()) return {};
		auto* session = found->second->playbackSession;
		if (!session || session->ended || session->playbackId != a_expectedPlayback) return {};
		AlignmentSnapshot result{session->playbackId, session->GetPlaybackSnapshot(), session->anchored, {}};
		const auto& stage = session->stages[result.playback.stage];
		for (std::size_t i = 0; i < session->participants.size(); ++i) {
			const auto& graph = session->participants[i];
			std::scoped_lock graphLock{graph->stateLock};
			if (!graph->target || graph->playbackSession != session) return {};
			const auto offset = graph->alignmentSegmentRevision == result.playback.segmentRevision ?
				graph->alignmentOffset : ParticipantPlacement{};
			result.participants.push_back({graph->target->GetFormID(), graph->roleName,
				stage.participants[i].file, stage.placements[i], offset});
		}
		return result;
	}

	bool GraphManager::UpdateAlignment(RE::Actor* a_actor, PlaybackId a_expectedPlayback,
		std::uint64_t a_segmentRevision, const std::vector<ParticipantPlacement>& a_offsets, std::optional<float> a_speed)
	{
		if (!a_expectedPlayback || !a_segmentRevision ||
			(a_speed && (!std::isfinite(*a_speed) || *a_speed < 0 || *a_speed > 4)) ||
			!std::ranges::all_of(a_offsets, ValidAlignmentOffset)) return false;
		std::vector<std::function<void()>> deferred;
		{
			std::unique_lock lock{stateLock};
			const auto found = graphs.find(a_actor);
			if (found == graphs.end()) return false;
			auto* session = found->second->playbackSession;
			if (!session || session->ended || session->playbackId != a_expectedPlayback) return false;
			const auto snapshot = session->GetPlaybackSnapshot();
			if (snapshot.segmentRevision != a_segmentRevision ||
				(!a_offsets.empty() && (!session->anchored || a_offsets.size() != session->participants.size()))) return false;
			for (const auto& graph : session->participants) {
				std::scoped_lock graphLock{graph->stateLock};
				if (!graph->target || graph->playbackSession != session || graph->appliedStage != snapshot.stage) return false;
			}
			// All validation precedes mutation. The session's stage data stays immutable.
			for (std::size_t i = 0; i < a_offsets.size(); ++i) {
				const auto graph = session->participants[i];
				std::scoped_lock graphLock{graph->stateLock};
				graph->alignmentOffset = a_offsets[i];
				graph->alignmentSegmentRevision = a_segmentRevision;
				++graph->anchorRevision;
				QueueAlignmentPlacement(*graph, deferred);
			}
			if (a_speed) session->speed.store(*a_speed, std::memory_order_relaxed);
		}
		Util::NativeMainThreadQueue::PostBatch(std::move(deferred));
		REX::DEBUG("[Anim] alignment updated for playback {} segment revision {}", a_expectedPlayback, a_segmentRevision);
		return true;
	}

	void GraphManager::QueueAlignmentPlacement(Graph& a_graph, std::vector<std::function<void()>>& a_deferred)
	{
		auto* session = a_graph.playbackSession;
		if (!session || !session->anchored || session->ended || !a_graph.target) return;
		const auto found = graphs.find(a_graph.target.get());
		if (found == graphs.end() || found->second.get() != &a_graph) return;
		const auto graph = found->second;
		const auto revision = graph->anchorRevision;
		const auto segmentRevision = graph->alignmentSegmentRevision;
		const auto playback = session->playbackId;
		const auto effective = AddPlacement(graph->scenePlacement, graph->alignmentOffset);
		const auto position = PlacementToWorld(session->anchorPos, session->anchorHeading, effective);
		const auto heading = session->anchorHeading + effective.heading;
		const auto epoch = session->worldEpoch;
		RE::NiPointer<RE::Actor> actor{static_cast<RE::Actor*>(graph->target.get())};
		a_deferred.emplace_back([actor, graph, revision, position, heading, epoch, playback, segmentRevision] {
			auto& manager = GetSingleton();
			if (manager._worldEpoch.load(std::memory_order_acquire) != epoch) return;
			{
				std::shared_lock state{manager.stateLock};
				const auto current = manager.graphs.find(actor.get());
				if (current == manager.graphs.end() || current->second != graph) return;
				std::scoped_lock guard{graph->stateLock};
				if (!graph->playbackSession || graph->playbackSession->ended || graph->anchorRevision != revision ||
					graph->playbackSession->playbackId != playback ||
					graph->playbackSession->GetPlaybackSnapshot().segmentRevision != segmentRevision) return;
			}
			actor->SetPosition(position, true);
			if (auto* transforms = RE::TransformService::GetSingleton()) transforms->SetHeadingZ(actor.get(), heading);
		});
	}
}
