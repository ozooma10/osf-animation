// Runs inside the private smoke runner, on its verified frame drain.
struct FoundationState
{
	const API::OSFEngineAPI* engine{};
	std::array<RE::NiPoint3, 2> baseline{};
	RE::NiPoint3 aligned{};
	std::vector<RE::TESFormID> worn;
	std::uint32_t input{};
	void* camera{};
	int visits{};
	bool playerRestoreSeen{}, peerRestoreSeen{};
	RE::NiPoint3 playerRestorePosition{}, peerRestorePosition{};
} foundation;

Json SceneControls()
{
	Json result;
	if (foundation.engine) foundation.engine->ReadSceneControls(g.scene, [](const char* text, void* context) {
		*static_cast<Json*>(context) = Json::parse(text, nullptr, false);
	}, &result);
	return result.is_object() ? result : Json::object();
}
bool NearPosition(const RE::NiPoint3& a, const RE::NiPoint3& b)
{
	return std::abs(a.x-b.x) < 0.01f && std::abs(a.y-b.y) < 0.01f && std::abs(a.z-b.z) < 0.01f;
}
bool FoundationRestored()
{
	auto* camera = RE::PlayerCamera::GetSingleton();
	const auto player = g.actor->GetPosition(), peer = feature.peer->GetPosition();
	const auto position = [](const RE::NiPoint3& p) { return Json::array({p.x,p.y,p.z}); };
	const bool input = InputFlags() == foundation.input, equipment = Worn(g.actor.get()) == foundation.worn;
	const bool cameraRestored = camera && camera->currentState == foundation.camera;
	const bool playerRestored = NearPosition(player, foundation.baseline[0]), peerRestored = NearPosition(peer, foundation.baseline[1]);
	// Restoration runs while the outgoing pose is still fading. Once the scene releases
	// its cast, the NPC's normal AI may move again before the visual tail disappears.
	const bool released = g.sceneAPI->GetSceneForActor(g.actor.get()) == 0 && g.sceneAPI->GetSceneForActor(feature.peer.get()) == 0;
	if (released && playerRestored) { foundation.playerRestoreSeen = true; foundation.playerRestorePosition = player; }
	if (released && peerRestored) { foundation.peerRestoreSeen = true; foundation.peerRestorePosition = peer; }
	g.snapshot["foundationRestoration"] = {{"input",input},{"equipment",equipment},{"camera",cameraRestored},
		{"player",playerRestored},{"peer",peerRestored},{"playerExpected",position(foundation.baseline[0])},
		{"playerObserved",position(player)},{"peerExpected",position(foundation.baseline[1])},{"peerObserved",position(peer)},
		{"playerRestoreSeen",foundation.playerRestoreSeen},{"peerRestoreSeen",foundation.peerRestoreSeen},
		{"playerRestorePosition",position(foundation.playerRestorePosition)},{"peerRestorePosition",position(foundation.peerRestorePosition)},
		{"inputExpected",foundation.input},{"inputObserved",InputFlags()},{"wornExpected",foundation.worn},{"wornObserved",Worn(g.actor.get())},
		{"cameraExpected",reinterpret_cast<std::uintptr_t>(foundation.camera)},
		{"cameraObserved",camera ? reinterpret_cast<std::uintptr_t>(camera->currentState) : 0}};
	return input && equipment && cameraRestored && foundation.playerRestoreSeen && foundation.peerRestoreSeen;
}
void AdvanceFoundation()
{
	auto* actor = g.actor.get();
	auto* peer = feature.peer.get();
	auto& manager = Animation::GraphManager::GetSingleton();
	auto* engine = foundation.engine;
	switch (g.phase) {
	case 200: {
		if (!WaitFeature("foundation.query", !manager.IsPlaying(actor) && !manager.IsPlaying(peer), 8, "Previous cast did not retire")) break;
		foundation = {};
		foundation.engine = API::RequestEngineAPI(); engine = foundation.engine;
		if (!Check("foundation.query", engine != nullptr, "Current services ABI 4.0 negotiated")) return;
		if (!Check("foundation.query", Registry::ContentRegistry::GetSingleton().Find("osf.example.handoff.start") &&
			Registry::ContentRegistry::GetSingleton().Find("osf.example.handoff.target"), "Both exact author example scenes loaded with installed vanilla assets")) return;
		foundation.baseline = {actor->GetPosition(), peer->GetPosition()};
		foundation.worn = Worn(actor); foundation.input = InputFlags();
		foundation.camera = RE::PlayerCamera::GetSingleton()->currentState;
		Scene::Alignment::SetFileForSmoke(g.directory / "alignment-overrides.json"); feature.privateAlignment = true;
		if (!g.callback) g.callback = g.sceneAPI->RegisterSceneEventCallback(SceneEvent, nullptr);
		{ std::lock_guard lock(g.eventsMutex); g.events.clear(); }
		RE::Actor* cast[]{actor, peer}; const char* tags[]{"osf-smoke-foundation-start"}; const char* roles[]{"lead", "partner"};
		API::OSFSceneQuery query; query.allOf = tags; query.allOfCount = 1; query.roles = roles; query.roleCount = 2;
		API::OSFSceneMatch match;
		auto options = Options(); options.followActorMode = 0; options.hasAnchor = true;
		options.anchorX = foundation.baseline[0].x; options.anchorY = foundation.baseline[0].y; options.anchorZ = foundation.baseline[0].z;
		options.anchorHeadingRad = actor->data.angle.z; options.hideApparelMode = options.playerInputLockMode = 1;
		std::memcpy(options.camera, "scene_orbit", 12);
		const bool found = g.sceneAPI->FindSceneByQuery(cast, 2, query, match);
		g.scene = g.sceneAPI->StartSceneByQuery(cast, 2, query, options);
		if (!Check("foundation.query", found && std::string_view(match.nodeId) == "osf.smoke.foundation.start/held" &&
			match.roleIndex[0] == 0 && match.roleIndex[1] == 1 && g.scene && g.callback,
			"Public query find/start selected the tagged scene with the requested actor-to-role mapping")) return;
		Phase(209); break;
	}
	case 209:
		// Speed and manual progression act on the published playback, which exists once the start is prepared.
		if (!AwaitSettled("foundation.manual-hold", 6, "Query-started scene did not finish preparation within 6 seconds")) break;
		if (!engine->SetSceneSpeed(g.scene, 0.5f) || !engine->SetSceneAutoAdvance(g.scene, false)) {
			Check("foundation.manual-hold", false, "Could not arm persistent speed/manual progression"); return;
		}
		Phase(201); break;
	case 201: {
		if (!WaitFeature("foundation.manual-hold", PoseStamped(actor) && PoseStamped(peer) &&
			Worn(actor) == WornAfterFullHide(actor) && Worn(actor) != foundation.worn &&
			!(InputFlags() & static_cast<std::uint32_t>(RE::USER_EVENT_FLAG::Movement)), 6, "Anchored cast did not engage playback and policy")) break;
		const float saved[]{0.125f,0,0,0}, working[]{0.25f,0,0,0};
		if (!engine->SetSceneAlignment(g.scene, "lead", saved) || !engine->SaveSceneAlignment(g.scene) ||
			!engine->SetSceneAlignment(g.scene, "lead", working)) {
			Check("foundation.alignment-reload", false, "Could not write private alignment fixture"); return;
		}
		// Discard the storage cache and read bytes again; the active working copy must remain 0.25.
		Scene::Alignment::SetFileForSmoke(g.directory / "alignment-overrides.json");
		float delta[4]{};
		const auto stored = Scene::Alignment::Read("osf.smoke.foundation.start/held", {"lead"});
		if (!Check("foundation.alignment-reload", stored.size() == 1 && stored[0].x == 0.125f &&
			engine->GetSceneAlignment(g.scene, "lead", delta) && delta[0] == 0.25f,
			"Atomic save survives a cleared storage cache without overwriting an active unsaved delta")) return;
		Phase(202); break;
	}
	case 202: {
		if (PhaseSeconds() < 2.5f) break;
		const auto controls = SceneControls();
		if (!Check("foundation.manual-hold", controls.value("node", "") == "osf.smoke.foundation.start/held" && !controls.value("automatic", true) &&
			Count(g.scene, API::SceneEventType::kCue, "foundation.repeat") >= 2 &&
			Count(g.scene, API::SceneEventType::kCue, "foundation.once") == 1,
			"Manual hold outlasted the timer while sampling and repeating cues continued; once cue fired once")) return;
		foundation.aligned = actor->GetPosition();
		const auto displacement = foundation.aligned - foundation.baseline[0];
		if (!Check("foundation.alignment-revisit", std::abs(displacement.Length() - 0.25f) < 0.01f,
			"The alignment delta moved the live actor capsule by the requested distance")) return;
		if (!engine->SetSceneSpeed(g.scene, 0) || !g.sceneAPI->Navigate(g.scene, "next")) {
			Check("foundation.alignment-revisit", false, "Paused navigation failed"); return;
		}
		Phase(203); break;
	}
	case 203:
		if (PhaseSeconds() < 0.15f || !AwaitSettled("foundation.alignment-revisit", 6, "Paused node transition did not finish preparation")) break;
		if (!g.sceneAPI->Navigate(g.scene, "back")) { Check("foundation.alignment-revisit", false, "Return edge failed"); return; }
		Phase(204); break;
	case 204: {
		if (PhaseSeconds() < 0.15f || !AwaitSettled("foundation.alignment-revisit", 6, "Return transition did not finish preparation")) break;
		float delta[4]{}; const auto controls = SceneControls();
		if (!Check("foundation.alignment-revisit", engine->GetSceneAlignment(g.scene, "lead", delta) && delta[0] == 0.25f &&
			NearPosition(actor->GetPosition(), foundation.aligned) && engine->GetSceneSpeed(g.scene) == 0 &&
			!controls.value("automatic", true), "Node revisits retain one alignment delta, a frozen anchor, pause and manual preference")) return;
		if (++foundation.visits < 3) { g.sceneAPI->Navigate(g.scene, "next"); Phase(203); break; }
		const char* tags[]{"osf-smoke-foundation-target"}; const char* roles[]{"partner", "lead"};
		API::OSFSceneQuery query; query.allOf = tags; query.allOfCount = 1; query.roles = roles; query.roleCount = 2;
		if (!Check("foundation.handoff", engine->ReplaceSceneByQuery(g.scene, &query), "Matchmade same-cast handoff accepted role reversal")) return;
		Phase(205); break;
	}
	case 205: {
		if (PhaseSeconds() < 0.2f || !AwaitSettled("foundation.handoff", 6, "Handoff did not finish preparation")) break;
		std::array<RE::Actor*,2> cast{}; const auto controls = SceneControls();
		if (!Check("foundation.handoff", g.sceneAPI->GetSceneParticipants(g.scene, cast.data(), 2) == 2 && cast[0] == peer && cast[1] == actor &&
			controls.value("node", "") == "osf.smoke.foundation.target/finish" && !controls.value("automatic", true) &&
			engine->GetSceneSpeed(g.scene) == 0 && Worn(actor) != foundation.worn &&
			!(InputFlags() & static_cast<std::uint32_t>(RE::USER_EVENT_FLAG::Movement)),
			"Handoff retained handle, pause, manual preference, apparel ownership and input lock while reversing the roster")) return;
		bool ordered{};
		{ std::lock_guard lock(g.eventsMutex);
			if (g.events.size() >= 3) {
				const auto n = g.events.size(); const auto& exit = g.events[n-3]; const auto& handoff = g.events[n-2]; const auto& enter = g.events[n-1];
				ordered = exit.type == API::SceneEventType::kNodeExit && exit.node == "osf.smoke.foundation.start/held" &&
					handoff.type == API::SceneEventType::kNodeTransition && handoff.edge == exit.node &&
					handoff.node == "osf.smoke.foundation.target/finish" && enter.type == API::SceneEventType::kNodeEnter && enter.node == handoff.node;
			}
		}
		if (!Check("foundation.handoff-events", ordered && Count(g.scene, API::SceneEventType::kSceneBegin) == 1 &&
			Count(g.scene, API::SceneEventType::kSceneEnd) == 0, "Old exit, handoff and new enter carry correct identities with no extra begin/end")) return;
		const char* badRoles[]{"missing", "lead"};
		if (!Check("foundation.handoff-rejected", !engine->ReplaceNode(g.scene, "osf.smoke.no-such-node", nullptr, 0, nullptr) &&
			!engine->ReplaceNode(g.scene, "osf.smoke.foundation.start/held", badRoles, 2, nullptr) &&
			SceneControls().value("node", "") == "osf.smoke.foundation.target/finish" &&
			Count(g.scene, API::SceneEventType::kNodeTransition) == 7, "Rejected targets and role bindings preserve the active identity and lifecycle")) return;
		engine->SetSceneSpeed(g.scene, 0.5f); Phase(206); break;
	}
	case 206:
		if (PhaseSeconds() < 1.2f) break;
		if (!Check("foundation.manual-resume", g.sceneAPI->GetSceneForActor(actor) == g.scene &&
			engine->SetSceneAutoAdvance(g.scene, true), "Handoff retained manual hold beyond its timer; automatic progression re-enabled")) return;
		Phase(207); break;
	case 207: {
		const bool restored = FoundationRestored();
		if (!WaitFeature("foundation.manual-resume", !manager.IsPlaying(actor) && !manager.IsPlaying(peer), 8, "Re-enabled target did not complete")) break;
		if (!WaitFeature("foundation.restore", restored, 8, "Role-reversal completion did not restore original actor positions, apparel, input and camera; see foundationRestoration")) break;
		if (!Check("foundation.restore", true, "Natural completion after role reversal restored each original actor position, apparel, input and camera")) return;
		{
			bool completed{}; { std::lock_guard lock(g.eventsMutex); completed = !g.events.empty() && g.events.back().type == API::SceneEventType::kSceneEnd &&
				g.events.back().cause == 0 && g.events.back().node == "osf.smoke.foundation.target/finish"; }
			if (!Check("foundation.manual-resume", completed, "Re-enabled progression ended the target once with completed cause")) return;
			RE::Actor* cast[]{actor, peer}; auto options = Options(); options.followActorMode = 0;
			g.scene = g.sceneAPI->StartScene(cast, 2, "osf.smoke.foundation.start/held", options);
			float delta[4]{};
			if (!Check("foundation.alignment-reload", g.scene && engine->GetSceneAlignment(g.scene, "lead", delta) && delta[0] == 0.125f,
				"A new scene reads the saved delta instead of the previous scene's unsaved value")) return;
			Phase(210);
		}
		break;
	}
	case 210:
		// Reset/save refuse while the fresh start is still preparing.
		if (!AwaitSettled("foundation.alignment-reload", 6, "Restarted scene did not finish preparation within 6 seconds")) break;
		if (!engine->ResetSceneAlignment(g.scene, "lead") || !engine->SaveSceneAlignment(g.scene)) {
			Check("foundation.alignment-reload", false, "Private alignment reset/save failed"); return;
		}
		Scene::Alignment::SetFileForSmoke(g.directory / "alignment-overrides.json");
		if (!Check("foundation.alignment-reload", Scene::Alignment::Read("osf.smoke.foundation.start/held", {"lead"})[0].x == 0,
			"Saved reset removes the persisted override on reload")) return;
		g.sceneAPI->StopScene(g.scene); Phase(208);
		break;
	case 208:
		if (!WaitFeature("foundation.restore", !manager.IsPlaying(actor) && !manager.IsPlaying(peer), 8, "Explicit stop retained cast playback")) break;
		if (!Check("foundation.restore", InputFlags() == foundation.input && Worn(actor) == foundation.worn &&
			Count(g.scene, API::SceneEventType::kSceneEnd) == 1, "Explicit stop cleaned up the restarted scene exactly once")) return;
		Scene::Alignment::SetFileForSmoke({}); feature.privateAlignment = false;
		g.scene = 0; Phase(183); break;
	}
}
