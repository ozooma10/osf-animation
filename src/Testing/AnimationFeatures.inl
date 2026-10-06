// Included in the private smoke runner's anonymous namespace. All mutations use its frame queue.
struct FeatureState
{
	std::mutex mutex;
	std::string papyrusRun;
	std::string papyrusVersion;
	bool papyrusReady{};
	std::int32_t papyrusToken{};
	std::vector<Event> papyrusEvents;
	std::vector<RE::TESFormID> worn;
	std::uint32_t inputFlags{};
	void* cameraState{};
	int importIndex{};
	Props::Instance prop;
	bool orbitOwned{};
	bool fadeOwned{};
	bool soundOwned{};
	bool subtitleOwned{};
	const OSFPersistenceAPI* persistence{};
	bool persistenceRegistered{};
	std::atomic<unsigned> saves{}, loads{}, reverts{}, resets{};
	std::atomic<std::uint32_t> value{0x51A7E123}, loadedValue{}, resolvedPlayer{};
	std::uint32_t voice{};
	std::string peerRun;
	RE::NiPointer<RE::Actor> peer;
	bool peerRetired{};
	bool browserRegistered{};
	Json browserReport=Json::object();
	Clock::time_point browserProbeAt{};
	int resumePhase{};
} feature;
constexpr std::uint64_t kSmokeVoice = 0x4F5346534D4F4B45ull;
constexpr std::uint32_t kSmokeClient = OSF_PERSISTENCE_FOURCC('O','A','T','S');

void BrowserObserved(const char*,const char* payload,const char* view,void*) noexcept
{
	try {
		if (!view || std::string_view(view) != BrowserProbe::kViewId) return;
		auto value=Json::parse(payload ? payload : "",nullptr,false);
		if (!value.is_object()) return;
		std::lock_guard lock(feature.mutex); feature.browserReport=std::move(value);
	} catch (...) {}
}
Json BrowserReport()
{
	if (Clock::now() >= feature.browserProbeAt) {
		feature.browserProbeAt=Clock::now()+std::chrono::milliseconds(500);
		const auto payload=Json{{"runId",g.snapshot["runId"]}}.dump();
		BrowserProbe::Views().EmitEvent(BrowserProbe::kViewId,"osf.animation.smoke.probe",payload.c_str());
	}
	std::lock_guard lock(feature.mutex);
	if (feature.browserReport.value("runId",std::string{}) != g.snapshot["runId"].get<std::string>()) return Json::object();
	return feature.browserReport;
}

void PapyrusReport(RE::BSScript::IVirtualMachine&, std::uint32_t, std::monostate,
	RE::BSFixedString run, bool ready, RE::BSFixedString version, std::int32_t token)
{
	std::lock_guard lock(feature.mutex);
	feature.papyrusRun = run.c_str(); feature.papyrusReady = ready;
	feature.papyrusVersion = version.c_str(); feature.papyrusToken = token;
}
void PapyrusEvent(RE::BSScript::IVirtualMachine&, std::uint32_t, std::monostate,
	std::int32_t scene, std::int32_t type, RE::BSFixedString cue)
{
	std::lock_guard lock(feature.mutex);
	if (feature.papyrusEvents.size() < 256) feature.papyrusEvents.push_back({scene,type,"",cue.c_str()});
}
void PapyrusPeer(RE::BSScript::IVirtualMachine&, std::uint32_t, std::monostate, RE::BSFixedString run, RE::Actor* actor)
{
	std::lock_guard lock(feature.mutex); feature.peerRun=run.c_str(); feature.peer.reset(actor);
}
void PapyrusRetired(RE::BSScript::IVirtualMachine&, std::uint32_t, std::monostate, RE::BSFixedString run)
{
	std::lock_guard lock(feature.mutex); if (feature.peerRun == run.c_str()) feature.peerRetired=true;
}
bool PeerCommand(bool retire)
{
	auto* game=RE::GameVM::GetSingleton(); auto* vm=game ? game->GetVM() : nullptr;
	if (!vm) return false;
	vm->BindNativeMethod("OSFAnimationSmoke","ReportPeer",&PapyrusPeer,true,false);
	vm->BindNativeMethod("OSFAnimationSmoke","ReportRetired",&PapyrusRetired,true,false);
	const std::string run=g.snapshot["runId"].get<std::string>();
	RE::NiPointer<RE::Actor> peer;
	{ std::lock_guard lock(feature.mutex); peer=feature.peer; }
	return vm->DispatchStaticCall("OSFAnimationSmoke",retire ? "RetirePeer" : "SpawnPeer",[run,peer,retire](RE::BSScrapArray<RE::BSScript::Variable>& args) {
		args.clear(); args.resize(retire ? 2 : 1); RE::BSScript::PackVariable(args[0],RE::BSFixedString(run));
		if (retire) RE::BSScript::PackVariable(args[1],peer.get()); return true;
	},RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>(),0);
}
bool ProbePapyrus()
{
	auto* game = RE::GameVM::GetSingleton();
	auto* vm = game ? game->GetVM() : nullptr;
	if (!vm) return false;
	RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> type;
	if (!vm->GetScriptObjectType("OSFAnimationSmoke", type) || !type) return false;
	vm->BindNativeMethod("OSFAnimationSmoke", "Report", &PapyrusReport, true, false);
	vm->BindNativeMethod("OSFAnimationSmoke", "ReportEvent", &PapyrusEvent, true, false);
	const std::string run = g.snapshot["runId"].get<std::string>();
	{
		std::lock_guard lock(feature.mutex);
		feature.papyrusRun.clear(); feature.papyrusEvents.clear();
	}
	return vm->DispatchStaticCall("OSFAnimationSmoke", "Probe", [run](RE::BSScrapArray<RE::BSScript::Variable>& args) {
		args.clear(); args.resize(1); RE::BSScript::PackVariable(args[0], RE::BSFixedString(run)); return true;
	}, RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>(), 0);
}
std::vector<RE::TESFormID> Worn(RE::Actor* actor)
{
	std::vector<RE::TESFormID> result;
	actor->ForEachEquippedItem([&](const RE::BGSInventoryItem& item) {
		if (item.object) result.push_back(item.object->GetFormID());
		return RE::BSContainer::ForEachResult::kContinue;
	});
	std::ranges::sort(result);
	return result;
}
std::uint32_t InputFlags()
{
	auto* input = RE::BSInputEnableManager::GetSingleton();
	if (!input) return 0;
	RE::BSAutoLock guard(input->m_cacheLock);
	return static_cast<std::uint32_t>(input->m_cachedUserEventFlags);
}
bool PoseStamped(RE::Actor* actor, bool additive = false)
{
	const auto observed = Animation::GraphManager::GetSingleton().ObservePoseForSmoke(actor);
	return observed && observed->boundBones > 0 && observed->stamped && observed->finite &&
		(!additive || observed->mode == Animation::PoseMode::kAdditive);
}
bool WaitFeature(const char* name, bool ready, float seconds, const char* failure)
{
	if (ready) return true;
	if (PhaseSeconds() > seconds) Check(name, false, failure);
	return false;
}
void ExternalAction(const char* action)
{
	g.snapshot["externalAction"] = action;
	g.snapshot["externalActionExpiresAtUtcMs"] = UtcMs() + 15000;
	g.snapshot["sessionId"] = g.session;
	Publish();
}
std::uint8_t OSF_PERSISTENCE_CALL SaveProbe(void*, OSFRecordWriterV1* writer)
{
	const std::uint32_t data[]{feature.value.load(), 0x14};
	const bool ok = writer && writer->OpenRecord(writer,kSmokeClient,1) && writer->WriteRecordData(writer,data,sizeof(data));
	if (ok) ++feature.saves;
	return ok;
}
std::uint8_t OSF_PERSISTENCE_CALL LoadProbe(void*, OSFRecordReaderV1* reader)
{
	std::uint32_t type{},version{},length{},data[2]{};
	if (!reader || !reader->GetNextRecordInfo(reader,&type,&version,&length) || type != kSmokeClient || version != 1 ||
		length != sizeof(data) || reader->ReadRecordData(reader,data,sizeof(data)) != sizeof(data)) return false;
	std::uint32_t resolved{};
	if (!reader->ResolveFormID(reader,data[1],&resolved)) return false;
	feature.loadedValue = data[0]; feature.resolvedPlayer = resolved; ++feature.loads;
	return true;
}
void OSF_PERSISTENCE_CALL RevertProbe(void*) { ++feature.reverts; }
void CleanupFeatures(bool worldReset)
{
	bool retirePeer{};
	{ std::lock_guard lock(feature.mutex); retirePeer = feature.peer && !feature.peerRetired; }
	if (!worldReset && retirePeer) PeerCommand(true);
	if (!feature.prop.Empty()) (void)Props::PropService::GetSingleton().Destroy(feature.prop);
	if (feature.orbitOwned) Camera::CameraService::GetSingleton().ReleaseInspectionOrbit();
	if (feature.fadeOwned) UI::FadeService::GetSingleton().FadeFromBlack(0.1f);
	if (feature.soundOwned) Audio::SoundService::GetSingleton().StopAll();
	if (feature.subtitleOwned) UI::Subtitle::OnStopAll();
	feature.orbitOwned = feature.fadeOwned = feature.soundOwned = feature.subtitleOwned = false;
	{
		std::int32_t token{};
		{ std::lock_guard lock(feature.mutex); token = std::exchange(feature.papyrusToken, 0); }
		if (token) Scene::SceneEventRelay::GetSingleton().Unregister(token);
	}
	if (feature.persistenceRegistered) feature.persistence->UnregisterClient(feature.persistence,kSmokeClient,&feature);
	feature.persistenceRegistered = false;
}
void FailFeature(const char* description)
{
	// A failed feature must not hide independent later features. Foundational
	// API/browser failures and load failures still stop: their prerequisites are shared.
	const int phase = g.phase;
	const int next = phase >= 100 && phase <= 102 ? 110 :
		phase >= 110 && phase <= 112 ? 120 : phase >= 120 && phase <= 121 ? 130 : phase >= 130 && phase <= 132 ? 135 :
		phase >= 135 && phase <= 137 ? 140 : phase >= 140 && phase <= 143 ? 160 :
		phase >= 160 && phase <= 161 ? 180 :
		phase >= 180 && phase <= 184 ? 170 : 0;
	if (!next) { Finish("failed", description); return; }
	Cleanup();
	g.actor.reset(RE::PlayerCharacter::GetSingleton());
	feature.resumePhase = next;
	g.snapshot["externalAction"] = "";
	Phase(90);
	Publish();
}
void RecoverFeature()
{
	if (!g.actor || g.actor.get() != RE::PlayerCharacter::GetSingleton()) {
		Finish("blocked", "Player changed while cleaning up a failed feature", true); return;
	}
	if (!ActorUnclaimed(g.actor.get())) {
		if (PhaseSeconds() > 10) Finish("failed", "Failed feature did not release its playback; remaining cases cannot safely run");
		return;
	}
	Phase(feature.resumePhase);
}
void AdvanceFeatures()
{
	auto* actor = g.actor.get();
	auto& manager = Animation::GraphManager::GetSingleton();
	auto& registry = Registry::ContentRegistry::GetSingleton();
	switch (g.phase) {
	case 100:
		if (!Check("papyrus.binding", ProbePapyrus(), "Smoke script and native report functions dispatch through the real VM")) return;
		Phase(101); break;
	case 101: {
		bool ready{};
		{
			std::lock_guard lock(feature.mutex);
			ready = feature.papyrusRun == g.snapshot["runId"].get<std::string>() && feature.papyrusReady &&
				!feature.papyrusVersion.empty() && feature.papyrusToken != 0;
		}
		if (!WaitFeature("papyrus.binding",ready,8,"OSF.IsReady/GetVersion/callback registration did not return through Papyrus")) break;
		if (!Check("papyrus.binding",true,"Shipped OSF.pex natives return readiness/version and a callback token through Papyrus")) return;
		auto options = Options();
		g.scene = g.sceneAPI->StartScene(&actor,1,"osf.smoke.graph",options);
		if (!g.scene || !g.sceneAPI->Navigate(g.scene,"next") || !g.sceneAPI->Advance(g.scene)) { Check("papyrus.callbacks",false,"Could not start the Papyrus event fixture"); return; }
		Phase(102); break;
	}
	case 102: {
		bool delivered{};
		{
			std::lock_guard lock(feature.mutex);
			delivered = std::ranges::count_if(feature.papyrusEvents,[&](const auto& e) { return e.handle == g.scene && e.type == API::SceneEventType::kCue && e.cue == "smoke.tick"; }) == 1 &&
				std::ranges::count_if(feature.papyrusEvents,[&](const auto& e) { return e.handle == g.scene && e.type == API::SceneEventType::kSceneEnd; }) == 1;
		}
		if (!WaitFeature("papyrus.callbacks",delivered && !manager.IsPlaying(actor),8,"Papyrus did not receive the cue and end SceneEvent structs")) break;
		if (!Check("papyrus.callbacks",true,"OSFTypes:SceneEvent reached a compiled Papyrus handler with the correct handle, cue and end event")) return;
		g.scene = 0; feature.importIndex = 0; Phase(110); break;
	}
	case 110: {
		constexpr const char* files[]{"OSF/Smoke/head.gltf","OSF/Smoke/head.glb","OSF/Smoke/head-gzip.glb"};
		constexpr const char* cases[]{"import.gltf","import.glb","import.gzip"};
		if (!Check(cases[feature.importIndex],manager.PlayAnimation(actor,files[feature.importIndex],"smoke"),"Generated clip loads through the production live playback path")) return;
		g.soloOwned = true; Phase(111); break;
	}
	case 111: {
		constexpr const char* cases[]{"import.gltf","import.glb","import.gzip"};
		if (!WaitFeature(cases[feature.importIndex],PoseStamped(actor),5,"Imported animation did not bind and stamp finite transforms into the game rig")) break;
		if (!Check(cases[feature.importIndex],true,"Generated format bound to the real actor rig and stamped finite transforms")) return;
		manager.StopAnimation(actor); Phase(112); break;
	}
	case 112:
		if (!WaitFeature("solo.stop",!manager.IsPlaying(actor),6,"Generated clip failed to fade out")) break;
		g.soloOwned = false;
		if (++feature.importIndex < 3) Phase(110); else Phase(120);
		break;
	case 120: {
		auto options = Options(); g.scene = g.sceneAPI->StartScene(&actor,1,"osf.smoke.additive",options);
		if (!Check("pose.additive-mask",g.scene != 0,"Masked additive scene starts")) return;
		Phase(121); break;
	}
	case 121:
		if (!WaitFeature("pose.additive-mask",PoseStamped(actor,true),5,"Masked additive graph did not stamp finite transforms")) break;
		if (!Check("pose.additive-mask",true,"Production additive sampler/stamper ran on a masked live rig")) return;
		g.sceneAPI->StopScene(g.scene); Phase(130); break;
	case 130: {
		if (!WaitFeature("pose.additive-mask", !manager.IsPlaying(actor), 6, "Additive scene did not clean up")) break;
		g.scene = 0;
		Matchmaking::TagQuery query; query.allOf={"osf-smoke-only"};
		const auto match=Matchmaking::Pick({actor},query);
		query.noneOf={"osf-smoke-only"};
		if (!Check("matchmaking.tags",match && match->id == "osf.smoke.match" && match->order == std::vector<std::size_t>{0} && !Matchmaking::Pick({actor},query),"Live actor matchmaking honors positive/negative tags and complete role binding")) return;
		const auto files=registry.FileStats();
		if (!Check("registry.import-records",std::ranges::any_of(files,[](const auto& f) { return f.file == "animation-features.osf.json" && f.scenes == 3 && f.errors == 0; }),"Every extended fixture was accepted and attributed to its import record")) return;
		feature.worn=Worn(actor); feature.inputFlags=InputFlags();
		if (feature.worn.empty() || !(feature.inputFlags & static_cast<std::uint32_t>(RE::USER_EVENT_FLAG::Movement))) { Finish("blocked","Equipment/input fixture requires a clothed player with movement initially enabled"); return; }
		auto options=Options(); g.scene=g.sceneAPI->StartScene(&actor,1,"osf.smoke.mechanisms",options);
		if (!g.scene) { Check("mechanisms.engage",false,"Mechanism scene did not start"); return; }
		Phase(131); break;
	}
	case 131:
		if (!WaitFeature("mechanisms.engage",!(InputFlags() & static_cast<std::uint32_t>(RE::USER_EVENT_FLAG::Movement)) && Worn(actor).size() < feature.worn.size(),5,"Authored lock/strip actions did not disable movement and remove worn items")) break;
		if (!Check("mechanisms.engage",true,"Authored actions changed the engine input mask and actual equipped inventory")) return;
		g.sceneAPI->StopScene(g.scene); Phase(132); break;
	case 132:
		if (!WaitFeature("mechanisms.restore",!manager.IsPlaying(actor) && InputFlags() == feature.inputFlags && Worn(actor) == feature.worn,8,"Scene cancellation failed to restore input masks and the worn item set")) break;
		if (!Check("mechanisms.restore",true,"Undo ledger restored exact input masks and worn form IDs after cancellation")) return;
		g.scene=0; Phase(135); break;
	case 135: {
		auto* camera=RE::PlayerCamera::GetSingleton(); feature.cameraState=camera ? camera->currentState : nullptr;
		feature.orbitOwned=Camera::CameraService::GetSingleton().AcquireInspectionOrbit({actor->GetFormID()});
		if (!Check("camera.orbit",feature.orbitOwned,"Browser orbit acquired on the current game build")) return;
		Phase(136); break;
	}
	case 136: {
		float position[3]{},forward[3]{};
		auto* camera=RE::PlayerCamera::GetSingleton();
		const bool active=camera && camera->currentState == camera->cameraStates[RE::CameraState::kFreeFly] &&
			Camera::CameraService::GetSingleton().SceneOrbitPose(position,forward) &&
			std::ranges::all_of(position,[](float v){return std::isfinite(v);});
		if (!WaitFeature("camera.orbit",active,5,"Orbit did not enter the native camera state and produce a finite pose")) break;
		if (!Check("camera.orbit",true,"Native camera entered FreeFly and the orbit driver produced a finite pose")) return;
		Camera::CameraService::GetSingleton().ReleaseInspectionOrbit(); feature.orbitOwned=false; Phase(137); break;
	}
	case 137: {
		auto* camera=RE::PlayerCamera::GetSingleton();
		if (!WaitFeature("camera.restore",camera && camera->currentState == feature.cameraState,5,"Orbit release failed to restore the previous native camera state")) break;
		if (!Check("camera.restore",true,"Camera state returned to its captured baseline")) return;
		Props::Source source;
		for (auto id : feature.worn) if ((id >> 24) == 0 && RE::TESForm::LookupByID<RE::TESObjectARMO>(id)) { source.form=std::format("Starfield.esm|0x{:X}",id); break; }
		if (source.form.empty()) { Finish("blocked","Prop fixture requires one worn vanilla armor form in the disposable seed"); return; }
		std::string error; Props::Attachment attachment; attachment.targetNode="C_Head"; attachment.scale=0.15f;
		feature.prop=Props::PropService::GetSingleton().CreateAttached(actor,source,attachment,&error);
		if (!Check("props.attach",!feature.prop.Empty() && feature.prop.object->parent != nullptr,"Prop service cloned a vanilla model and attached it to the live actor hierarchy")) return;
		attachment.targetNode="R_Wrist";
		if (!Check("props.reattach",Props::PropService::GetSingleton().Attach(feature.prop,actor,attachment,&error) && feature.prop.object->parent != nullptr,"Existing prop moved to a different live attachment node")) return;
		if (!Check("props.destroy",Props::PropService::GetSingleton().Destroy(feature.prop,&error) && feature.prop.Empty() && Props::PropService::GetSingleton().Destroy(feature.prop,&error),"Prop detach/destruction clears ownership and is idempotent")) return;
		Phase(140); break;
	}
	case 140:
		feature.fadeOwned=UI::FadeService::GetSingleton().FadeToBlack(0.1f,0.25f);
		if (!Check("fade.bounded",feature.fadeOwned && UI::FadeService::GetSingleton().HoldingForSmoke(),"Verified fade poster accepted a bounded fade hold")) return;
		UI::Subtitle::Show(actor,"OSF Animation automated subtitle test",0.4f); feature.subtitleOwned=true;
		if (!Check("subtitle.deadline",UI::Subtitle::HoldingForSmoke(),"Subtitle acquired a bounded display lifetime")) return;
		Phase(141); break;
	case 141:
		if (!WaitFeature("fade.bounded",!UI::FadeService::GetSingleton().HoldingForSmoke() && !UI::Subtitle::HoldingForSmoke(),4,"Frame maintenance did not release fade/subtitle deadlines")) break;
		if (!Check("fade.bounded",true,"Normal frame maintenance expired the fade latch without a test-driven Tick")) return;
		if (!Check("subtitle.deadline",true,"Normal frame maintenance expired the subtitle display lifetime")) return;
		feature.fadeOwned=feature.subtitleOwned=false;
		if (!Check("sound.pool",Registry::SoundRegistry::GetSingleton().Resolve("$osf-smoke") == std::optional<std::string>{"OSF/Smoke/tone.wav"},"Runtime tagged sound registry resolved the staged tone")) return;
		feature.soundOwned=true; Audio::SoundService::GetSingleton().Play(kSmokeVoice,"OSF/Smoke/tone.wav"); Phase(142); break;
	case 142:
		feature.voice=Audio::SoundService::GetSingleton().VoiceForSmoke(kSmokeVoice);
		if (!WaitFeature("sound.wwise",feature.voice != 0,8,"Audio worker/Wwise did not return a playing ID for the generated WAV")) break;
		if (!Check("sound.wwise",true,"Production worker decoded the WAV and Wwise accepted the external source with a playing ID")) return;
		Audio::SoundService::GetSingleton().Play(kSmokeVoice,"OSF/Smoke/tone.wav"); Phase(143); break;
	case 143: {
		const auto current=Audio::SoundService::GetSingleton().VoiceForSmoke(kSmokeVoice);
		if (!WaitFeature("sound.replace-stop",current != 0 && current != feature.voice,8,"A second sound did not replace the actor voice slot")) break;
		Audio::SoundService::GetSingleton().StopAll(); feature.soundOwned=false;
		if (!Check("sound.replace-stop",Audio::SoundService::GetSingleton().VoiceForSmoke(kSmokeVoice) == 0,"Second voice replaced the first; teardown cleared the live slot")) return;
		Phase(160); break;
	}
	case 160: {
		if (!WaitFeature("scene.files-roles",!manager.IsPlaying(actor),6,"Previous playback retained its graph after stop")) break;
		auto options=Options(); const char* file=kClip;
		g.scene=g.sceneAPI->StartSceneFiles(&actor,&file,1,options);
		if (!Check("scene.files-roles",g.scene && g.sceneAPI->SetSpeed(actor,0),"Ad-hoc files scene supports shared-clock pause")) return;
		const auto playback=manager.GetScenePlayback(actor);
		if (!Check("scene.transport",playback && playback->duration > 0 && std::isfinite(playback->time) && playback->speed == 0 && g.sceneAPI->SetSpeed(actor,1),"Shared scene clock reports pause and accepts resume")) return;
		g.sceneAPI->StopSceneForActor(actor); Phase(161); break;
	}
	case 161: {
		if (!WaitFeature("scene.files-roles",!manager.IsPlaying(actor),6,"Ad-hoc files scene failed to stop")) break;
		auto options=Options(); const char* role="player";
		g.scene=g.sceneAPI->StartSceneRoles(&actor,1,"osf.smoke.linear",&role,1,options);
		if (!Check("scene.files-roles",g.scene && g.sceneAPI->GetSceneParticipants(g.scene,nullptr,0) == 1,"Explicit role-name binding started the correct roster")) return;
		g.sceneAPI->StopScene(g.scene); Phase(180); break;
	}
	case 180:
		if (!WaitFeature("multi-actor.spawn",!manager.IsPlaying(actor),6,"Role scene did not release before actor fixture")) break;
		g.scene=0;
		{ std::lock_guard lock(feature.mutex); feature.peer.reset(); feature.peerRun.clear(); feature.peerRetired=false; }
		if (!Check("multi-actor.spawn",PeerCommand(false),"Requested a new disposable actor through the Papyrus placement API")) return;
		Phase(181); break;
	case 181: {
		RE::NiPointer<RE::Actor> peer;
		{ std::lock_guard lock(feature.mutex); if (feature.peerRun == g.snapshot["runId"].get<std::string>()) peer=feature.peer; }
		bool loaded{};
		if (peer) { const auto data=peer->loadedData.LockRead(); loaded=*data && (*data)->data3D; }
		if (!WaitFeature("multi-actor.spawn",loaded,15,"Disposable actor did not acquire live 3D")) break;
		if (!Check("multi-actor.spawn",peer.get() != actor,"A distinct, newly placed actor has a loaded rig")) return;
		RE::Actor* actors[]{actor,peer.get()}; const char* clips[]{kClip,kClip}; auto options=Options();
		g.scene=g.sceneAPI->StartSceneFiles(actors,clips,2,options);
		if (!g.scene) { Check("multi-actor.sync",false,"Two-actor files scene failed"); return; }
		Phase(182); break;
	}
	case 182: {
		auto* peer=feature.peer.get();
		if (!WaitFeature("multi-actor.sync",PoseStamped(actor) && PoseStamped(peer),8,"Both participants did not stamp into their own live rigs")) break;
		const auto first=manager.GetScenePlayback(actor),second=manager.GetScenePlayback(peer);
		if (!Check("multi-actor.sync",first && second && first->playbackId == second->playbackId && std::abs(first->time-second->time) < 0.001f && g.sceneAPI->GetSceneParticipants(g.scene,nullptr,0) == 2,"Two stamped actor rigs share one playback ID and clock")) return;
		if (!Check("multi-actor.speed",g.sceneAPI->SetSpeed(peer,0) && g.sceneAPI->GetSpeed(actor) == 0 && g.sceneAPI->SetSpeed(actor,1.5f) && std::abs(g.sceneAPI->GetSpeed(peer)-1.5f) < 0.001f,"Speed changes through either participant affect the same shared clock")) return;
		g.sceneAPI->StopScene(g.scene); Phase(183); break;
	}
	case 183:
		if (!WaitFeature("multi-actor.cleanup",!manager.IsPlaying(actor) && !manager.IsPlaying(feature.peer.get()),8,"Scene stop retained a participant graph")) break;
		g.scene=0;
		if (!Check("multi-actor.cleanup",PeerCommand(true),"Both graphs retired; disposable actor disable/delete requested")) return;
		Phase(184); break;
	case 184: {
		bool retired{}; { std::lock_guard lock(feature.mutex); retired=feature.peerRetired; }
		if (!WaitFeature("multi-actor.cleanup",retired,10,"Disposable actor deletion did not complete through Papyrus")) break;
		if (!Check("multi-actor.cleanup",true,"Papyrus returned after disabling and deleting the disposable actor")) return;
		{ std::lock_guard lock(feature.mutex); feature.peer.reset(); }
		Phase(170); break;
	}
	case 170: {
		if (!WaitFeature("persistence.save",!manager.IsPlaying(actor),6,"Scene graph remained before persistence test")) break;
		g.scene=0;
		feature.persistence=OSF_RequestPersistenceAPI(OSF_PERSISTENCE_API_VERSION);
		feature.saves=0; feature.loads=0; feature.reverts=0; feature.resets=0;
		feature.value=0x51A7E123; feature.loadedValue=0; feature.resolvedPlayer=0;
		OSFPersistenceClientV1 client{sizeof(OSFPersistenceClientV1),kSmokeClient,"Animation acceptance",&feature,SaveProbe,LoadProbe,RevertProbe,nullptr};
		feature.persistenceRegistered=feature.persistence && feature.persistence->IsReady(feature.persistence) && feature.persistence->RegisterClient(feature.persistence,&client);
		if (!Check("persistence.save",feature.persistenceRegistered,"Private acceptance persistence client registered with the live host")) return;
		ExternalAction("quicksave"); Phase(171); break;
	}
	case 171: {
		if (!WaitFeature("persistence.save",feature.saves.load() > 0,30,"Real quicksave did not invoke the registered record writer")) break;
		if (!Check("persistence.save",true,"Real quicksave called the client writer and serialized the probe record")) return;
		feature.value=0xBAD; // The load must restore the saved value, not the current live one.
		auto options=Options(); g.scene=g.sceneAPI->StartScene(&actor,1,"osf.smoke.linear",options);
		if (!g.scene) { Check("persistence.world-reset",false,"Could not arm live playback before quickload"); return; }
		ExternalAction("quickload"); Phase(172); break;
	}
	case 172:
		if (!WaitFeature("persistence.load-remap",feature.loads.load() > 0 && feature.resets.load() > 0 && RE::PlayerCharacter::GetSingleton(),45,"Real quickload did not restore the sidecar or replace the world")) break;
		g.actor.reset(RE::PlayerCharacter::GetSingleton()); actor=g.actor.get(); g.scene=0;
		if (!Check("persistence.load-remap",feature.loadedValue == 0x51A7E123 && feature.resolvedPlayer == actor->GetFormID() && feature.reverts > 0,"Load restored the saved record after revert and resolved the player FormID")) return;
		if (!Check("persistence.world-reset",!manager.IsPlaying(actor) && Scene::SceneRuntime::GetSingleton().ListScenes().empty(),"Actual world replacement cleared active playback and runtime ownership")) return;
		ExternalAction("");
		if (!Check("papyrus.rebind",ProbePapyrus(),"Smoke script dispatch accepted after VM rebuild")) return;
		Phase(173); break;
	case 173: {
		bool ready{};
		{ std::lock_guard lock(feature.mutex); ready=feature.papyrusReady && feature.papyrusRun == g.snapshot["runId"].get<std::string>(); }
		if (!WaitFeature("papyrus.rebind",ready,10,"OSF natives did not return from the rebuilt VM")) break;
		if (!Check("papyrus.rebind",true,"OSF readiness native executed through Papyrus after a real quickload")) return;
		Finish("passed","All implemented runtime assertions passed; coverage report retains visual, hardware and external integration requirements");
		break;
	}
	}
}
