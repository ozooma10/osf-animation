#include "Testing/D3AudioProbe.h"

#ifdef OSF_ANIMATION_SMOKE
#include "API/OSFSceneAPI.h"
#include "API/OSFEngineAPI.h"
#include "Audio/SoundPlayback.h"
#include "Audio/SoundService.h"
#include "Registry/ContentRegistry.h"
#include <chrono>
#include <condition_variable>

namespace OSF::Testing::D3AudioProbe
{
    namespace
    {
        using Json = nlohmann::json;
        std::mutex mutex;
        std::condition_variable gate;
        std::atomic<std::uint64_t> drain{0};
        thread_local std::uint64_t current{};
        bool active{}, released{}, waiting{}, timedOut{};
        bool scoped{};
        std::uint64_t voiceChannel{};
        std::array<std::uint64_t,3> testChannels{};
        std::uint64_t gateId{}, callback{}, pending{}, resetCount{};
        std::int32_t scene{};
        std::string trial, gatePoint;
        std::vector<Json> samples;
        Json events = Json::array();
        std::chrono::steady_clock::time_point deadline;
        API::IOSFSceneAPI* api{};

        void CloseVoice()
        {
            std::uint64_t channel;
            { std::lock_guard lock(mutex); channel = std::exchange(voiceChannel,0); }
            if (channel) API::RequestEngineAPI()->CloseAudioChannel(channel);
        }

        void Event(const char* name)
        {
            std::lock_guard lock(mutex);
            if (active) events.push_back({{"name",name},{"atNs",NowNs()},{"drain",drain.load()}});
        }

        void OnScene(const API::OSFSceneEvent* e, void*)
        {
            if (!e) return;
            {
                std::lock_guard lock(mutex);
                if (!active || e->sceneHandle != scene) return;
            }
            if (e->eventType == API::SceneEventType::kSceneEnd) Event("scene-end");
            if (e->eventType == API::SceneEventType::kSceneHandoff) Event("scene-handoff");
            if (e->eventType == API::SceneEventType::kNodeEnter) Event("node-enter");
            if (e->eventType == API::SceneEventType::kSceneEnd ||
                e->eventType == API::SceneEventType::kSceneHandoff ||
                e->eventType == API::SceneEventType::kNodeEnter) CloseVoice();
            if (e->eventType != API::SceneEventType::kCue) return;
            const std::string_view cue = e->cue ? e->cue : "";
            if (!cue.starts_with("d3:OSF/D3/")) return;
            Point(Current(), "callbackNs");
            Point(Current(), "callbackDrain", drain.load());
            Audio::SoundPlayback::Play(e->actor, e->sceneHandle, cue.substr(3),
                Registry::SoundEmitter::kListener, "private D3 callback");
        }

        Json Snapshot()
        {
            Json result;
            std::array<std::uint64_t,3> channels;
            {
                std::lock_guard lock(mutex);
                result = {{"trial",trial},{"active",active},{"scene",scene},{"pending",pending},
                    {"gate",gatePoint},{"waiting",waiting},{"released",released},{"gateId",gateId},
                    {"gateTimedOut",timedOut},{"worldResets",resetCount},{"samples",samples},{"events",events},{"scoped",scoped}};
                channels = testChannels;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            result["playerPlaying"] = api && player && api->IsPlaying(player);
            result["channels"] = Json::array();
            for (const auto channel : channels)
                result["channels"].push_back({{"handle",channel},
                    {"playingID",Audio::SoundService::GetSingleton().ChannelVoiceForSmoke(channel)}});
            return result;
        }
    }

    std::uint64_t NowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void FrameDrain() { ++drain; }
    std::uint64_t Current() { return current; }
    void BeginMark(std::int32_t handle, std::uint64_t playback, std::string_view node,
        std::uint8_t lane, std::string_view token, std::uint64_t crossed)
    {
        current = 0;
        std::lock_guard lock(mutex);
        if (!active || handle != scene || (lane != 2 && lane != 3) || samples.size() >= 512) return;
        current = samples.size() + 1;
        samples.push_back({{"id",current},{"scene",handle},{"playback",playback},{"node",node},
            {"lane",lane},{"token",token},{"crossedNs",crossed},{"dispatchNs",NowNs()},
            {"dispatchDrain",drain.load()}});
    }
    void EndMark() { current = 0; }
    bool TryScopedSound(std::string_view spec)
    {
        std::uint64_t channel;
        {
            std::lock_guard lock(mutex);
            if (!active || !scoped || !current || !spec.starts_with("OSF/D3/")) return false;
            channel = voiceChannel;
        }
        const std::string path(spec);
        if (!API::RequestEngineAPI()->PlayAudioChannel(channel,path.c_str())) Point(current,"submissionRefused",1);
        return true;
    }
    void VoiceStopped(std::uint32_t playing)
    {
        std::lock_guard lock(mutex);
        if (active) events.push_back({{"name","voice-stop"},{"playingID",playing},{"atNs",NowNs()}});
    }
    std::uint64_t CaptureAudio(std::string_view path, std::uint64_t slot)
    {
        std::lock_guard lock(mutex);
        if (!active || !path.starts_with("OSF/D3/")) return 0;
        auto id = current;
        if (!id) {
            if (samples.size() >= 512) return 0;
            id = samples.size() + 1;
            samples.push_back({{"id",id},{"auxiliary",true}});
        }
        auto& sample = samples.at(id - 1);
        sample["path"] = path; sample["slot"] = slot; sample["enqueueNs"] = NowNs();
        if (!gateId && !gatePoint.empty() && !sample.value("auxiliary",false)) gateId = id;
        ++pending;
        return id;
    }
    void Point(std::uint64_t id, const char* name, std::uint64_t value)
    {
        if (!id) return;
        std::lock_guard lock(mutex);
        samples.at(id - 1)[name] = value ? value : NowNs();
    }
    void Checkpoint(std::uint64_t id, const char* name)
    {
        if (!id) return;
        std::unique_lock lock(mutex);
        if (id != gateId || gatePoint != name || released) return;
        waiting = true;
        samples.at(id - 1)["gateEnteredNs"] = NowNs();
        // Never block the game thread. Only audio-worker call sites use this gate.
        if (!gate.wait_for(lock, std::chrono::seconds(45), [] { return released; })) {
            timedOut = true; released = true;
        }
        waiting = false;
        samples.at(id - 1)["gateReleasedNs"] = NowNs();
    }
    AudioContext::AudioContext(std::uint64_t value) : id(value), previous(current) { current = id; }
    AudioContext::~AudioContext()
    {
        if (id) {
            std::lock_guard lock(mutex);
            samples.at(id - 1)["completeNs"] = NowNs();
            --pending;
        }
        current = previous;
    }
    void WorldReset() noexcept
    {
        try {
            std::lock_guard lock(mutex);
            if (!active) return;
            ++resetCount;
            events.push_back({{"name","world-reset"},{"atNs",NowNs()},{"drain",drain.load()}});
            scene = 0;
            voiceChannel = 0; // The engine's world teardown has revoked all channels.
        } catch (...) {}
    }
    void Tick()
    {
        bool expired = false;
        {
            std::lock_guard lock(mutex);
            expired = active && std::chrono::steady_clock::now() > deadline && !released;
            if (expired) { released = true; timedOut = true; }
        }
        if (expired) gate.notify_all();
    }
    Json Command(const std::string& command, const Json& args)
    {
        if (command == "d3-status") return Snapshot();
        if (command == "d3-release") {
            { std::lock_guard lock(mutex); released = true; }
            Event("release"); gate.notify_all(); return Snapshot();
        }
        if (command == "d3-channels") {
            const auto* services = API::RequestEngineAPI();
            const auto action = args.at("action").get<std::string>();
            if (action == "start") {
                {
                    std::lock_guard lock(mutex);
                    if (pending || waiting || voiceChannel) throw std::runtime_error("D3 audio still in flight");
                    active = true; scoped = false; scene = 0; trial = "independent-channels";
                    gatePoint.clear(); gateId = 0; timedOut = false; resetCount = 0;
                    samples.clear(); events = Json::array();
                }
                const char* paths[] = {"OSF/D3/voice.wav","OSF/D3/contact.wav","OSF/D3/other.wav"};
                for (std::size_t i=0; i<3; ++i) {
                    testChannels[i] = services->CreateAudioChannel();
                    if (!testChannels[i] || !services->PlayAudioChannel(testChannels[i],paths[i]))
                        throw std::runtime_error("D3 channel start refused");
                }
            } else if (action == "cancel") {
                if (!services->CloseAudioChannel(testChannels[0])) throw std::runtime_error("D3 voice close refused");
                Event("voice-cancelled");
                auto result = Snapshot();
                result["closedPlayRejected"] = !services->PlayAudioChannel(testChannels[0],"OSF/D3/voice.wav");
                result["repeatCloseRejected"] = !services->CloseAudioChannel(testChannels[0]);
                return result;
            } else if (action == "restart") {
                const auto retired = testChannels[0];
                testChannels[0] = services->CreateAudioChannel();
                if (!testChannels[0] || testChannels[0] == retired ||
                    !services->PlayAudioChannel(testChannels[0],"OSF/D3/voice.wav")) throw std::runtime_error("D3 fresh channel refused");
            } else if (action == "finish") {
                for (const auto channel : testChannels) services->CloseAudioChannel(channel);
            } else throw std::runtime_error("Unknown D3 channel action");
            return Snapshot();
        }
        if (command == "d3-start") {
            {
                std::lock_guard lock(mutex);
                if (pending || waiting) throw std::runtime_error("D3 previous audio is still in flight");
            }
            if (!api) api = API::RequestSceneAPI();
            const auto* services = API::RequestEngineAPI();
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!api || !services || !api->IsReady() || !player || !player->parentCell ||
                !RE::Main::GetSingleton() || RE::Main::GetSingleton()->isGameMenuPaused ||
                api->IsPlaying(player) || api->GetSceneForActor(player))
                throw std::runtime_error("D3 requires loaded unpaused gameplay and an unclaimed player");
            const auto id = args.at("scene").get<std::string>();
            if (id != "osf.d3.lane" && id != "osf.d3.beat" &&
                id != "osf.d3.cancel-lane" && id != "osf.d3.cancel-beat")
                throw std::runtime_error("D3 scene is outside fixture allowlist");
            const auto point = args.value("gate",std::string{});
            if (!point.empty() && point != "worker" && point != "during-prepare" && point != "before-post")
                throw std::runtime_error("Unknown D3 worker checkpoint");
            if (!callback) callback = api->RegisterSceneEventCallback(OnScene,nullptr);
            if (!callback) throw std::runtime_error("D3 callback registration failed");
            {
                std::lock_guard lock(mutex);
                active = true; released = false; waiting = false; timedOut = false;
                scoped = args.value("scoped",false); testChannels = {};
                trial = args.at("trial"); gatePoint = point; gateId = 0; resetCount = 0;
                samples.clear(); events = Json::array(); scene = 0;
                deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
            }
            API::OSFStartOptions options;
            options.stripMode = options.lockPlayerMode = options.playerControlMode = options.fadeMode = 0;
            options.inPlaceMode = 1;
            std::memcpy(options.camera,"none",5);
            auto* actor = static_cast<RE::Actor*>(player);
            const auto handle = api->StartScene(&actor,1,id.c_str(),options);
            { std::lock_guard lock(mutex); scene = handle; if (!handle) active = false; }
            if (!handle) throw std::runtime_error("D3 scene start refused");
            if (scoped) {
                const auto channel = services->CreateAudioChannel();
                { std::lock_guard lock(mutex); voiceChannel = channel; }
                if (!channel) throw std::runtime_error("D3 channel allocation refused");
            }
            const auto speed = args.value("speed",1.0f);
            if (!std::isfinite(speed) || speed < 0 || speed > 2 || !services->SetSceneSpeed(scene,speed))
                throw std::runtime_error("D3 speed refused");
            Event("start"); return Snapshot();
        }
        if (command == "d3-cancel") {
            Event("cancel");
            const auto mode = args.at("mode").get<std::string>();
            const auto* services = API::RequestEngineAPI();
            bool ok = false;
            if (mode == "stop") ok = scene && api->StopScene(scene);
            else if (mode == "handoff") ok = scene && services->ReplaceScene(scene,"osf.d3.silent",nullptr);
            else if (mode == "authored") ok = scene && api->Navigate(scene,"authored");
            else throw std::runtime_error("D3 cancellation mode must be stop/handoff/authored");
            if (!ok) throw std::runtime_error("D3 cancellation operation refused");
            Event("cancel-returned"); return Snapshot();
        }
        if (command == "d3-speed") {
            const auto value = args.at("speed").get<float>();
            if (!std::isfinite(value) || value < 0 || value > 2 ||
                !API::RequestEngineAPI()->SetSceneSpeed(scene,value))
                throw std::runtime_error("D3 speed refused");
            Event(value == 0 ? "pause" : "resume"); return Snapshot();
        }
        if (command == "d3-finish") {
            { std::lock_guard lock(mutex); released = true; }
            gate.notify_all();
            if (scene && api && api->GetSceneForActor(RE::PlayerCharacter::GetSingleton()) == scene)
                (void)api->StopScene(scene);
            CloseVoice();
            if (const auto* services = API::RequestEngineAPI())
                for (const auto channel : testChannels) services->CloseAudioChannel(channel);
            Event("finish"); return Snapshot();
        }
        throw std::runtime_error("Unknown D3 command");
    }
}
#endif
