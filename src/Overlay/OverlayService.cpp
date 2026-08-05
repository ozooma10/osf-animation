#include "Overlay/OverlayService.h"

#include "Animation/GraphManager.h"
#include "Audio/SoundPlayback.h"
#include "Scene/SceneRuntime.h"
#include "Util/StringUtil.h"

#include <algorithm>
#include <charconv>

namespace OSF::Overlay
{
	namespace
	{
		// CommonLibSF leaves RE::TESObjectLoadedEvent::GetEventSource bound to
		// REL::ID(0), and its nearby candidate 107177 is not a parameterless event
		// accessor. The engine source is a global BSTEventSource (the same direct
		// binding used by SFSE's GameEvents table). Validate its live vptr before use
		// so a stale Address Library entry disables only 3D tracking.
		constexpr REL::ID kObjectLoadedEventSourceGlobalID{ 838433 };
		constexpr REL::ID kObjectLoadedEventSourceVtableID{ 413689 };

		RE::BSTEventSource<RE::TESObjectLoadedEvent>* ObjectLoadedEventSource()
		{
			static auto* source = []() -> RE::BSTEventSource<RE::TESObjectLoadedEvent>* {
				auto* candidate = reinterpret_cast<RE::BSTEventSource<RE::TESObjectLoadedEvent>*>(
					kObjectLoadedEventSourceGlobalID.address());
				const auto liveVtable = candidate ?
					*reinterpret_cast<const std::uintptr_t*>(candidate) : 0;
				const auto expectedVtable = kObjectLoadedEventSourceVtableID.address();
				if (liveVtable != expectedVtable) {
					REX::WARN("[Anim] overlay actor-3D event tracking disabled: "
						"TESObjectLoadedEvent source vtable mismatch (global ID {}, vtable ID {})",
						kObjectLoadedEventSourceGlobalID.id(),
						kObjectLoadedEventSourceVtableID.id());
					return nullptr;
				}
				return candidate;
			}();
			return source;
		}

		constexpr std::uint8_t kLaneCommit = 0;
		constexpr std::uint8_t kLaneProp = 1;
		constexpr std::uint8_t kLaneSound = 2;
		constexpr std::uint8_t kLaneMarker = 3;
		constexpr std::uint8_t kLaneReached = 4;

		std::int32_t MakeHandle(std::uint16_t a_generation, std::uint16_t a_slot)
		{
			return static_cast<std::int32_t>((static_cast<std::uint32_t>(a_generation) << 16) | a_slot);
		}

		bool HasActor3D(RE::Actor* a_actor)
		{
			if (!a_actor) return false;
			const auto loaded = a_actor->loadedData.LockRead();
			return *loaded && (*loaded)->data3D;
		}

		std::optional<std::size_t> ParseIndex(std::string_view a_value)
		{
			std::size_t value = 0;
			const auto [end, error] = std::from_chars(a_value.data(), a_value.data() + a_value.size(), value);
			return error == std::errc{} && end == a_value.data() + a_value.size() ?
				std::optional<std::size_t>{ value } : std::nullopt;
		}

		API::OverlayReason PublicReason(RequestReason a_reason)
		{
			switch (a_reason) {
			case RequestReason::kNone: return API::OverlayReason::kNone;
			case RequestReason::kInvalidHandle: return API::OverlayReason::kInvalidHandle;
			case RequestReason::kUnknownStation: return API::OverlayReason::kUnknownStation;
			case RequestReason::kNoPath: return API::OverlayReason::kNoPath;
			case RequestReason::kBusy: return API::OverlayReason::kBusy;
			case RequestReason::kSceneBlocked: return API::OverlayReason::kSceneBlocked;
			case RequestReason::kActorUnavailable: return API::OverlayReason::kActorUnavailable;
			case RequestReason::kPlaybackFailed: return API::OverlayReason::kPlaybackFailed;
			case RequestReason::kHandoffRejected: return API::OverlayReason::kHandoffRejected;
			case RequestReason::kOwnerInvalid: return API::OverlayReason::kOwnerInvalid;
			case RequestReason::kRouteUnknown: return API::OverlayReason::kRouteUnknown;
			case RequestReason::kDispatchDeferred: return API::OverlayReason::kDispatchDeferred;
			}
			return API::OverlayReason::kPlaybackFailed;
		}

		API::OverlayPhase PublicPhase(ControllerPhase a_phase)
		{
			switch (a_phase) {
			case ControllerPhase::kAtStation: return API::OverlayPhase::kAtStation;
			case ControllerPhase::kTransitioning: return API::OverlayPhase::kTransitioning;
			case ControllerPhase::kFailed: return API::OverlayPhase::kFailed;
			}
			return API::OverlayPhase::kFailed;
		}

		API::OverlayBlocker PublicBlocker(ControllerBlocker a_blocker)
		{
			switch (a_blocker) {
			case ControllerBlocker::kNone: return API::OverlayBlocker::kNone;
			case ControllerBlocker::kScene: return API::OverlayBlocker::kScene;
			case ControllerBlocker::kActor3D: return API::OverlayBlocker::kActor3DUnavailable;
			}
			return API::OverlayBlocker::kNone;
		}
	}

	struct OverlayService::ActiveProp
	{
		std::string id;
		Registry::RouteLifetime lifetime = Registry::RouteLifetime::kTransition;
		std::string station;
		Props::Source source;
		Props::Attachment attachment;
		Props::Instance instance;
	};

	struct OverlayService::Slot
	{
		std::uint16_t generation = 0;
		std::uint64_t owner = 0;
		RE::NiPointer<RE::Actor> actor;
		Registry::RouteRef route;
		std::unique_ptr<PlaybackAdapter> playback;
		std::unique_ptr<RouteController> controller;
		std::vector<ActiveProp> props;
	};

	class OverlayService::PlaybackAdapter final : public IRoutePlayback
	{
	public:
		PlaybackAdapter(OverlayService& a_service, std::int32_t a_handle) :
			service(a_service), handle(a_handle)
		{}

		bool PlayStation(const Registry::RouteStation& a_station) override
		{
			auto* slot = service.Resolve(handle);
			if (!slot || !slot->actor) return false;
			if (!a_station.layer) {
				Stop(false);
				return true;
			}
			Animation::ScenePlan plan;
			plan.animId = slot->route->id + ":station:" + a_station.id;
			plan.anchored = false;
			plan.poseModes = { a_station.layer->mode };
			plan.poseWeights = { a_station.layer->weight };
			plan.masks = { a_station.layer->mask };
			Animation::ScenePlan::Stage stage;
			stage.files = { a_station.layer->clip.file };
			stage.animIds = { a_station.layer->clip.animId };
			stage.masks = { a_station.layer->mask };
			stage.poseModes = { a_station.layer->mode };
			stage.poseWeights = { a_station.layer->weight };
			stage.hold = a_station.layer->holdAt >= 0.0f ? a_station.layer->holdAt : 1.0f;
			plan.stages.push_back(std::move(stage));
			Animation::PlaybackId next = 0;
			if (!Animation::GraphManager::GetSingleton().PlaySceneStaged({ slot->actor.get() }, plan, 0,
				&next, playbackId, service._playbackSinkId, true)) {
				return false;
			}
			playbackId = next;
			return true;
		}

		bool PlayTransition(const Registry::RouteTransition& a_transition,
			const Registry::RouteStation& a_destination, std::uint32_t a_generation) override
		{
			auto* slot = service.Resolve(handle);
			if (!slot || !slot->actor) return false;
			service.CleanupProps(*slot, Registry::RouteLifetime::kTransition);
			service.CleanupProps(*slot, Registry::RouteLifetime::kStation);

			Animation::ScenePlan plan;
			plan.animId = slot->route->id + ":transition:" + a_transition.id;
			plan.anchored = false;
			plan.poseModes = { a_transition.layer.mode };
			plan.poseWeights = { a_transition.layer.weight };
			plan.masks = { a_transition.layer.mask };
			Animation::ScenePlan::Stage edge;
			edge.files = { a_transition.layer.clip.file };
			edge.animIds = { a_transition.layer.clip.animId };
			edge.masks = { a_transition.layer.mask };
			edge.poseModes = { a_transition.layer.mode };
			edge.poseWeights = { a_transition.layer.weight };
			if (a_transition.contactPose) edge.contactPose = { *a_transition.contactPose };
			// Route edges are authored motion between station poses. Applying the generic scene
			// blend here changes that motion (and can consume half of a short edge), rather than
			// merely smoothing entry into an unrelated scene.
			edge.blendIn = 0.0f;
			edge.loops = 1;
			if (a_transition.commit) {
				edge.marks.push_back({ .seconds = a_transition.commit->frame / Registry::kFrameRate,
					.lane = kLaneCommit, .token = a_transition.commit->id });
			}
			for (std::size_t i = 0; i < a_transition.props.size(); ++i) {
				edge.marks.push_back({ .seconds = a_transition.props[i].frame / Registry::kFrameRate,
					.lane = kLaneProp, .token = std::to_string(i) });
			}
			for (std::size_t i = 0; i < a_transition.sounds.size(); ++i) {
				edge.marks.push_back({ .seconds = a_transition.sounds[i].frame / Registry::kFrameRate,
					.lane = kLaneSound, .token = std::to_string(i) });
			}
			for (std::size_t i = 0; i < a_transition.markers.size(); ++i) {
				edge.marks.push_back({ .seconds = a_transition.markers[i].frame / Registry::kFrameRate,
					.lane = kLaneMarker, .token = std::to_string(i) });
			}
			edge.marks.push_back({ .atEnd = true, .lane = kLaneReached, .token = std::to_string(a_generation) });
			plan.stages.push_back(std::move(edge));
			if (a_destination.layer) {
				Animation::ScenePlan::Stage hold;
				hold.files = { a_destination.layer->clip.file };
				hold.animIds = { a_destination.layer->clip.animId };
				hold.masks = { a_destination.layer->mask };
				hold.poseModes = { a_destination.layer->mode };
				hold.poseWeights = { a_destination.layer->weight };
				hold.hold = a_destination.layer->holdAt >= 0.0f ? a_destination.layer->holdAt : 1.0f;
				plan.stages.push_back(std::move(hold));
			}
			Animation::PlaybackId next = 0;
			if (!Animation::GraphManager::GetSingleton().PlaySceneStaged({ slot->actor.get() }, plan, 0,
				&next, playbackId, service._playbackSinkId, true)) {
				return false;
			}
			playbackId = next;
			return true;
		}

		void Stop(bool a_fade) override
		{
			if (playbackId == 0) return;
			auto* slot = service.Resolve(handle);
			if (slot && slot->actor) {
				if (a_fade) Animation::GraphManager::GetSingleton().StopScene(slot->actor.get(), playbackId);
				else Animation::GraphManager::GetSingleton().StopSceneImmediate(slot->actor.get(), playbackId);
			}
			playbackId = 0;
		}

		OverlayService& service;
		std::int32_t handle = 0;
		Animation::PlaybackId playbackId = 0;
	};

	OverlayService::~OverlayService() = default;

	OverlayService& OverlayService::GetSingleton()
	{
		static OverlayService singleton;
		return singleton;
	}

	void OverlayService::Register()
	{
		std::lock_guard l{ _lock };
		if (_registered) return;
		Animation::GraphManager::PlaybackSink sink;
		sink.autoEnd = [](Animation::PlaybackId a_id, const std::vector<RE::Actor*>& a_actors,
			Animation::SceneEndReason a_reason) {
			return GetSingleton().OnAutoEnd(a_id, a_actors, a_reason);
		};
		sink.timedMarks = [](Animation::PlaybackId a_id, const std::vector<RE::Actor*>& a_actors,
			const std::vector<Animation::FiredMark>& a_marks) {
			GetSingleton().OnTimedMarks(a_id, a_actors, a_marks);
		};
		sink.clear = []() { GetSingleton().ClearWorld(); };
		_playbackSinkId = Animation::GraphManager::GetSingleton().RegisterPlaybackSink(std::move(sink));
		if (auto* source = ObjectLoadedEventSource()) source->RegisterSink(this);
		_registered = true;
	}

	std::uint64_t OverlayService::AcquireOwner(std::string_view a_pluginId,
		API::OSFOverlayCallback a_callback, void* a_context)
	{
		std::lock_guard l{ _lock };
		return _owners.Acquire(a_pluginId, a_callback, a_context);
	}

	bool OverlayService::ReleaseOwner(std::uint64_t a_owner)
	{
		std::unique_lock serviceLock{ _lock };
		if (_dispatchDepth != 0) {
			const bool found = _owners.MarkReleasing(a_owner);
			_deferredMutations.emplace_back([this, a_owner]() { (void)ReleaseOwner(a_owner); });
			return found;
		}
		if (!_owners.MarkReleasing(a_owner)) return false;
		std::vector<std::int32_t> handles;
		for (std::size_t i = 0; i < _slots.size(); ++i) {
			if (_slots[i] && _slots[i]->owner == a_owner) handles.push_back(MakeHandle(_slots[i]->generation, static_cast<std::uint16_t>(i)));
		}
		for (const auto handle : handles) (void)EndRoute(handle, false, false);
		serviceLock.unlock();
		return _owners.Release(a_owner);
	}

	std::int32_t OverlayService::MintHandle()
	{
		std::size_t index = 0;
		for (; index < _slots.size() && _slots[index]; ++index) {}
		if (index >= 0xFFFFu) return 0;
		if (index == _slots.size()) _slots.push_back(nullptr);
		std::uint16_t generation = _nextGeneration++;
		if (generation == 0) generation = _nextGeneration++;
		return MakeHandle(generation, static_cast<std::uint16_t>(index));
	}

	OverlayService::Slot* OverlayService::Resolve(std::int32_t a_handle)
	{
		if (a_handle == 0) return nullptr;
		const auto index = static_cast<std::uint16_t>(a_handle & 0xFFFF);
		const auto generation = static_cast<std::uint16_t>((static_cast<std::uint32_t>(a_handle) >> 16) & 0xFFFF);
		return index < _slots.size() && _slots[index] && _slots[index]->generation == generation ? _slots[index].get() : nullptr;
	}

	const OverlayService::Slot* OverlayService::Resolve(std::int32_t a_handle) const
	{
		return const_cast<OverlayService*>(this)->Resolve(a_handle);
	}

	BeginResult OverlayService::BeginRoute(std::uint64_t a_owner, RE::Actor* a_actor,
		std::string_view a_routeId, std::string_view a_initialStation)
	{
		if (!a_actor) return { 0, RequestDisposition::kRejected, RequestReason::kActorUnavailable };
		std::lock_guard l{ _lock };
		if (!_owners.IsUsable(a_owner)) return { 0, RequestDisposition::kRejected, RequestReason::kOwnerInvalid };
		if (_byActor.contains(a_actor)) return { 0, RequestDisposition::kRejected, RequestReason::kBusy };
		auto route = Registry::SceneRegistry::GetSingleton().FindRoute(a_routeId);
		if (!route) return { 0, RequestDisposition::kRejected, RequestReason::kRouteUnknown };
		if (!route->FindStation(a_initialStation)) {
			return { 0, RequestDisposition::kRejected, RequestReason::kUnknownStation };
		}
		const auto sceneHandle = Scene::SceneRuntime::GetSingleton().GetSceneForActor(a_actor);
		if (!sceneHandle && Animation::GraphManager::GetSingleton().IsPlaying(a_actor)) {
			return { 0, RequestDisposition::kRejected, RequestReason::kBusy };
		}
		const auto handle = MintHandle();
		if (!handle) return { 0, RequestDisposition::kRejected, RequestReason::kBusy };
		const auto index = static_cast<std::uint16_t>(handle & 0xFFFF);
		auto slot = std::make_unique<Slot>();
		slot->generation = static_cast<std::uint16_t>((static_cast<std::uint32_t>(handle) >> 16) & 0xFFFF);
		slot->owner = a_owner;
		slot->actor.reset(a_actor);
		slot->route = route;
		slot->playback = std::make_unique<PlaybackAdapter>(*this, handle);
		slot->controller = std::make_unique<RouteController>(*route, std::string(a_initialStation), *slot->playback);
		_slots[index] = std::move(slot);
		_byActor.emplace(a_actor, handle);
		if (sceneHandle) {
			_slots[index]->controller->SetBlocker(ControllerBlocker::kScene);
			_sceneSuspensions[sceneHandle].push_back(handle);
		}
		if (_dispatchDepth != 0) {
			_deferredMutations.emplace_back([this, handle]() { RealizeDeferredBegin(handle); });
			return { handle, RequestDisposition::kPending, RequestReason::kDispatchDeferred };
		}
		const auto result = StartRoute(handle);
		if (result.disposition == RequestDisposition::kRejected) {
			DropRoute(handle);
			return { 0, result.disposition, result.reason };
		}
		return { handle, result.disposition, result.reason };
	}

	RequestResult OverlayService::StartRoute(std::int32_t a_handle)
	{
		Slot* slot = Resolve(a_handle);
		if (!slot || !slot->actor) return { RequestDisposition::kRejected, RequestReason::kInvalidHandle };
		if (!HasActor3D(slot->actor.get())) slot->controller->SetBlocker(ControllerBlocker::kActor3D);
		const auto result = slot->controller->Start();
		if (result.disposition != RequestDisposition::kRejected) {
			REX::INFO("[Anim] overlay route '{}' started on actor {:X} (handle {:#010x}, station '{}')",
				slot->route->id, slot->actor->formID, a_handle, slot->controller->ReachedStation());
		}
		return result;
	}

	void OverlayService::RealizeDeferredBegin(std::int32_t a_handle)
	{
		Slot* slot = Resolve(a_handle);
		if (!slot) return;
		if (!_owners.IsUsable(slot->owner)) {
			DropRoute(a_handle);
			return;
		}
		const auto result = StartRoute(a_handle);
		if (result.disposition != RequestDisposition::kRejected) return;
		API::OSFOverlayEvent event;
		event.type = API::OverlayEventType::kFailed;
		event.routeHandle = a_handle;
		event.actor = slot->actor.get();
		event.route = slot->route->id.c_str();
		event.outcome = API::OverlayRequestDisposition::kRejected;
		event.reason = PublicReason(result.reason);
		(void)DispatchOwner(*slot, event, false);
		DropRoute(a_handle);
	}

	void OverlayService::DropRoute(std::int32_t a_handle)
	{
		Slot* slot = Resolve(a_handle);
		if (!slot) return;
		slot->controller->End(false);
		CleanupProps(*slot);
		_byActor.erase(slot->actor.get());
		const auto index = static_cast<std::uint16_t>(a_handle & 0xFFFF);
		_slots[index].reset();
	}

	RequestResult OverlayService::RequestStation(std::int32_t a_handle, std::string_view a_station, std::uint64_t a_token)
	{
		std::lock_guard l{ _lock };
		if (_dispatchDepth != 0) {
			const std::string station(a_station);
			_deferredMutations.emplace_back([this, a_handle, station, a_token]() { (void)RequestStation(a_handle, station, a_token); });
			return { RequestDisposition::kPending, RequestReason::kNone };
		}
		Slot* slot = Resolve(a_handle);
		return slot ? slot->controller->RequestStation(a_station, a_token) :
			RequestResult{ RequestDisposition::kRejected, RequestReason::kInvalidHandle };
	}

	bool OverlayService::EndRoute(std::int32_t a_handle, bool a_fade, bool a_notify)
	{
		std::lock_guard l{ _lock };
		if (_dispatchDepth != 0) {
			_deferredMutations.emplace_back([this, a_handle, a_fade, a_notify]() { (void)EndRoute(a_handle, a_fade, a_notify); });
			return Resolve(a_handle) != nullptr;
		}
		Slot* slot = Resolve(a_handle);
		if (!slot) return false;
		if (a_notify) {
			API::OSFOverlayEvent event;
			event.type = API::OverlayEventType::kEnded;
			event.routeHandle = a_handle;
			event.actor = slot->actor.get();
			event.route = slot->route->id.c_str();
			(void)DispatchOwner(*slot, event, false);
		}
		slot->controller->End(a_fade);
		CleanupProps(*slot);
		_byActor.erase(slot->actor.get());
		const auto index = static_cast<std::uint16_t>(a_handle & 0xFFFF);
		REX::INFO("[Anim] overlay route '{}' ended (handle {:#010x})", slot->route->id, a_handle);
		_slots[index].reset();
		DrainDeferredMutations();
		return true;
	}

	std::int32_t OverlayService::GetRouteForActor(RE::Actor* a_actor) const
	{
		std::lock_guard l{ _lock };
		const auto it = _byActor.find(a_actor);
		return it == _byActor.end() ? 0 : it->second;
	}

	bool OverlayService::QueryRoute(std::int32_t a_handle, API::OSFOverlayRouteState& a_out) const
	{
		std::lock_guard l{ _lock };
		const Slot* slot = Resolve(a_handle);
		if (!slot) return false;
		a_out = {};
		a_out.routeHandle = a_handle;
		a_out.actor = slot->actor.get();
		a_out.route = slot->route->id.c_str();
		a_out.reachedStation = slot->controller->ReachedStation().c_str();
		a_out.checkpointStation = slot->controller->CheckpointStation().c_str();
		a_out.desiredStation = slot->controller->DesiredStation().c_str();
		a_out.phase = PublicPhase(slot->controller->Phase());
		a_out.blocker = PublicBlocker(slot->controller->Blocker());
		a_out.requestToken = slot->controller->RequestToken();
		a_out.transitionGeneration = slot->controller->TransitionGeneration();
		return true;
	}

	bool OverlayService::DispatchOwner(Slot& a_slot, API::OSFOverlayEvent& a_event, bool a_commit)
	{
		auto lease = _owners.BeginDispatch(a_slot.owner);
		if (!lease) return !a_commit;
		++_dispatchDepth;
		const auto result = InvokeOwnerCallback(std::move(lease), a_event, a_commit);
		--_dispatchDepth;
		if (result.threw) {
			REX::ERROR("[API] overlay owner '{}' callback threw; exception isolated", result.plugin);
		}
		return a_commit ? result.acknowledged : true;
	}

	void OverlayService::DrainDeferredMutations()
	{
		while (_dispatchDepth == 0 && !_deferredMutations.empty()) {
			auto mutation = std::move(_deferredMutations.front());
			_deferredMutations.pop_front();
			mutation();
		}
	}

	void OverlayService::CleanupProps(Slot& a_slot, std::optional<Registry::RouteLifetime> a_lifetime)
	{
		for (auto it = a_slot.props.begin(); it != a_slot.props.end();) {
			if (a_lifetime && it->lifetime != *a_lifetime) {
				++it;
				continue;
			}
			std::string error;
			if (!Props::PropService::GetSingleton().Destroy(it->instance, &error)) {
				REX::ERROR("[Anim] overlay prop '{}' cleanup failed: {}", it->id, error);
			}
			it = a_slot.props.erase(it);
		}
	}

	void OverlayService::SuspendProps(Slot& a_slot)
	{
		for (auto it = a_slot.props.begin(); it != a_slot.props.end();) {
			std::string error;
			(void)Props::PropService::GetSingleton().Destroy(it->instance, &error);
			if (it->lifetime == Registry::RouteLifetime::kTransition) it = a_slot.props.erase(it);
			else ++it;
		}
	}

	void OverlayService::RestoreProps(Slot& a_slot)
	{
		if (!a_slot.actor || !HasActor3D(a_slot.actor.get())) return;
		for (auto& prop : a_slot.props) {
			if (!prop.instance.Empty()) continue;
			std::string error;
			prop.instance = Props::PropService::GetSingleton().CreateAttached(
				a_slot.actor.get(), prop.source, prop.attachment, &error);
			if (prop.instance.Empty()) REX::WARN("[Anim] overlay prop '{}' restore deferred: {}", prop.id, error);
		}
	}

	void OverlayService::PlayRouteSound(Slot& a_slot, std::string_view a_spec)
	{
		Audio::SoundPlayback::Play(a_slot.actor.get(), 0, a_spec, Registry::SoundEmitter::kListener,
			std::string("overlay '") + a_slot.route->id + "'");
	}

	void OverlayService::OnTimedMarks(Animation::PlaybackId a_playbackId,
		const std::vector<RE::Actor*>&, const std::vector<Animation::FiredMark>& a_marks)
	{
		std::lock_guard l{ _lock };
		Slot* slot = nullptr;
		std::int32_t handle = 0;
		for (std::size_t i = 0; i < _slots.size(); ++i) {
			if (_slots[i] && _slots[i]->playback->playbackId == a_playbackId) {
				slot = _slots[i].get();
				handle = MakeHandle(slot->generation, static_cast<std::uint16_t>(i));
				break;
			}
		}
		if (!slot) return;
		const auto* transition = slot->controller->ActiveTransition();
		if (!transition) return;
		const std::string transitionId = transition->id;
		const std::string from = transition->from;
		const std::string to = transition->to;
		const auto generation = slot->controller->TransitionGeneration();
		auto marks = a_marks;
		std::stable_sort(marks.begin(), marks.end(), [](const auto& a, const auto& b) { return a.lane < b.lane; });
		for (const auto& mark : marks) {
			if (!Resolve(handle) || slot->controller->TransitionGeneration() != generation) break;
			if (mark.lane == kLaneCommit) {
				API::OSFOverlayEvent event;
				event.type = API::OverlayEventType::kCommit;
				event.routeHandle = handle;
				event.actor = slot->actor.get();
				event.route = slot->route->id.c_str();
				event.fromStation = from.c_str();
				event.toStation = to.c_str();
				event.transition = transitionId.c_str();
				event.marker = mark.token.c_str();
				event.requestToken = slot->controller->RequestToken();
				event.transitionGeneration = generation;
				const bool ack = DispatchOwner(*slot, event, true);
				(void)slot->controller->OnCommit(generation, ack);
				if (!ack) {
					CleanupProps(*slot, Registry::RouteLifetime::kTransition);
					break;  // a rejected handoff invalidates every later lane in this frame
				}
			} else if (mark.lane == kLaneProp) {
				const auto index = ParseIndex(mark.token);
				if (!index || *index >= transition->props.size()) continue;
				const auto& prop = transition->props[*index];
				if (prop.lifetime == Registry::RouteLifetime::kExternal) {
					API::OSFOverlayEvent event;
					event.type = prop.attach ? API::OverlayEventType::kPropAttach : API::OverlayEventType::kPropDestroy;
					event.routeHandle = handle;
					event.actor = slot->actor.get();
					event.route = slot->route->id.c_str();
					event.fromStation = from.c_str();
					event.toStation = to.c_str();
					event.transition = transitionId.c_str();
					event.prop = prop.id.c_str();
					event.requestToken = slot->controller->RequestToken();
					event.transitionGeneration = generation;
					(void)DispatchOwner(*slot, event, false);
					continue;
				}
				if (prop.attach) {
					std::string error;
					auto instance = Props::PropService::GetSingleton().CreateAttached(slot->actor.get(), prop.source, prop.attachment, &error);
					if (instance.Empty()) REX::ERROR("[Anim] overlay prop '{}' attach failed: {}", prop.id, error);
					else slot->props.push_back({ prop.id, prop.lifetime, to, prop.source, prop.attachment, std::move(instance) });
				} else {
					for (auto it = slot->props.begin(); it != slot->props.end(); ++it) {
						if (Util::ToLower(it->id) != Util::ToLower(prop.id)) continue;
						std::string error;
						(void)Props::PropService::GetSingleton().Destroy(it->instance, &error);
						slot->props.erase(it);
						break;
					}
				}
			} else if (mark.lane == kLaneSound) {
				const auto index = ParseIndex(mark.token);
				if (index && *index < transition->sounds.size()) PlayRouteSound(*slot, transition->sounds[*index].spec);
			} else if (mark.lane == kLaneMarker) {
				const auto index = ParseIndex(mark.token);
				if (!index || *index >= transition->markers.size()) continue;
				API::OSFOverlayEvent event;
				event.type = API::OverlayEventType::kMarker;
				event.routeHandle = handle;
				event.actor = slot->actor.get();
				event.route = slot->route->id.c_str();
				event.fromStation = from.c_str();
				event.toStation = to.c_str();
				event.transition = transitionId.c_str();
				event.marker = transition->markers[*index].id.c_str();
				event.requestToken = slot->controller->RequestToken();
				event.transitionGeneration = generation;
				(void)DispatchOwner(*slot, event, false);
			} else if (mark.lane == kLaneReached) {
				CleanupProps(*slot, Registry::RouteLifetime::kTransition);
				(void)slot->controller->OnEdgeReached(generation);
				API::OSFOverlayEvent event;
				const bool failed = slot->controller->Phase() == ControllerPhase::kFailed ||
					slot->controller->LastReason() == RequestReason::kNoPath;
				event.type = failed ? API::OverlayEventType::kFailed : API::OverlayEventType::kReached;
				event.routeHandle = handle;
				event.actor = slot->actor.get();
				event.route = slot->route->id.c_str();
				event.fromStation = from.c_str();
				event.toStation = to.c_str();
				event.transition = transitionId.c_str();
				event.requestToken = slot->controller->RequestToken();
				event.transitionGeneration = generation;
				event.outcome = failed ? API::OverlayRequestDisposition::kRejected : API::OverlayRequestDisposition::kAccepted;
				event.reason = PublicReason(slot->controller->LastReason());
				(void)DispatchOwner(*slot, event, false);
			}
		}
		DrainDeferredMutations();
	}

	bool OverlayService::OnAutoEnd(Animation::PlaybackId a_playbackId,
		const std::vector<RE::Actor*>&, Animation::SceneEndReason)
	{
		std::lock_guard l{ _lock };
		for (auto& slot : _slots) {
			if (slot && slot->playback->playbackId == a_playbackId) {
				slot->playback->playbackId = 0;
				return false;  // GraphManager removes the exact retained playback
			}
		}
		return false;
	}

	void OverlayService::SuspendForScene(std::int32_t a_sceneHandle, const std::vector<RE::Actor*>& a_participants)
	{
		std::lock_guard l{ _lock };
		auto& suspended = _sceneSuspensions[a_sceneHandle];
		for (auto* actor : a_participants) {
			const auto it = _byActor.find(actor);
			if (it == _byActor.end()) continue;
			if (Slot* slot = Resolve(it->second)) {
				const bool changed = slot->controller->SetBlocker(ControllerBlocker::kScene);
				SuspendProps(*slot);
				suspended.push_back(it->second);
				if (!changed) continue;
				API::OSFOverlayEvent event;
				event.type = API::OverlayEventType::kSuspended;
				event.routeHandle = it->second;
				event.actor = slot->actor.get();
				event.route = slot->route->id.c_str();
				event.reason = API::OverlayReason::kSceneBlocked;
				(void)DispatchOwner(*slot, event, false);
			}
		}
		DrainDeferredMutations();
	}

	void OverlayService::ReconcileAfterScene(std::int32_t a_sceneHandle)
	{
		std::lock_guard l{ _lock };
		const auto found = _sceneSuspensions.find(a_sceneHandle);
		if (found == _sceneSuspensions.end()) return;
		for (const auto handle : found->second) {
			if (Slot* slot = Resolve(handle)) {
				const bool changed = slot->controller->ClearBlocker(ControllerBlocker::kScene);
				if (slot->controller->Blocker() == ControllerBlocker::kNone) RestoreProps(*slot);
				if (!changed || slot->controller->Blocker() != ControllerBlocker::kNone) continue;
				API::OSFOverlayEvent event;
				const bool failed = slot->controller->Phase() == ControllerPhase::kFailed;
				event.type = failed ? API::OverlayEventType::kFailed : API::OverlayEventType::kResumed;
				event.routeHandle = handle;
				event.actor = slot->actor.get();
				event.route = slot->route->id.c_str();
				event.outcome = failed ? API::OverlayRequestDisposition::kRejected : API::OverlayRequestDisposition::kAccepted;
				event.reason = PublicReason(slot->controller->LastReason());
				(void)DispatchOwner(*slot, event, false);
			}
		}
		_sceneSuspensions.erase(found);
		DrainDeferredMutations();
	}

	void OverlayService::ClearWorld()
	{
		std::lock_guard l{ _lock };
		for (auto& slot : _slots) if (slot) CleanupProps(*slot);
		_slots.clear();
		_byActor.clear();
		_sceneSuspensions.clear();
	}

	RE::BSEventNotifyControl OverlayService::ProcessEvent(const RE::TESObjectLoadedEvent& a_event,
		RE::BSTEventSource<RE::TESObjectLoadedEvent>*)
	{
		std::lock_guard l{ _lock };
		for (auto& slot : _slots) {
			if (!slot || !slot->actor || slot->actor->formID != a_event.formID) continue;
			const auto actorRoute = _byActor.find(slot->actor.get());
			if (actorRoute == _byActor.end()) continue;
			if (!a_event.loaded || !HasActor3D(slot->actor.get())) {
				const bool changed = slot->controller->SetBlocker(ControllerBlocker::kActor3D);
				SuspendProps(*slot);
				if (!changed) continue;
				API::OSFOverlayEvent event;
				event.type = API::OverlayEventType::kSuspended;
				event.routeHandle = actorRoute->second;
				event.actor = slot->actor.get();
				event.route = slot->route->id.c_str();
				event.reason = API::OverlayReason::kActorUnavailable;
				(void)DispatchOwner(*slot, event, false);
			} else {
				const bool changed = slot->controller->ClearBlocker(ControllerBlocker::kActor3D);
				if (slot->controller->Blocker() == ControllerBlocker::kNone) RestoreProps(*slot);
				if (!changed || slot->controller->Blocker() != ControllerBlocker::kNone) continue;
				API::OSFOverlayEvent event;
				const bool failed = slot->controller->Phase() == ControllerPhase::kFailed;
				event.type = failed ? API::OverlayEventType::kFailed : API::OverlayEventType::kResumed;
				event.routeHandle = actorRoute->second;
				event.actor = slot->actor.get();
				event.route = slot->route->id.c_str();
				event.outcome = failed ? API::OverlayRequestDisposition::kRejected : API::OverlayRequestDisposition::kAccepted;
				event.reason = PublicReason(slot->controller->LastReason());
				(void)DispatchOwner(*slot, event, false);
			}
		}
		DrainDeferredMutations();
		return RE::BSEventNotifyControl::kContinue;
	}
}
