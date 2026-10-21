#include "Testing/D3Qualification.h"
#ifdef OSF_ANIMATION_SMOKE
#include "Testing/D3AudioProbe.h"
#include "Testing/D3OutputBuffers.h"
#include "Testing/D3QualificationState.h"
#include "API/OSFEngineAPI.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>

namespace OSF::Testing::D3Qualification {
namespace {
    using Json=nlohmann::json;
    using Callback=void(*)(void*,std::uint32_t,void*);
    using Register=std::uint32_t(*)(Callback,std::uint32_t,void*,std::uint32_t,std::uint32_t,std::uint32_t);
    using OutputID=std::uint64_t(*)(std::uint32_t,std::uint32_t);
    using Remove=std::uint32_t(*)(std::uint64_t);
    using Add=std::uint32_t(*)(const void*,std::uint64_t*,const std::uint64_t*,std::uint32_t);
    using Suspend=std::uint32_t(*)(bool);
    using Wake=std::uint32_t(*)();
    Register registerCallback{}; OutputID outputID{}; Remove removeOutput{}; Add addOutput{};
    Suspend suspendOutput{}; Wake wakeOutput{};
    struct Frame {std::atomic<bool> ready{}; std::uint64_t begin{},end{};std::uint32_t generation{},thread{};};
    struct Lifecycle {std::atomic<bool> ready{};std::uint64_t at{};std::uint32_t location{},generation{},thread{};};
    std::array<Frame,32768> frames;
    std::array<Lifecycle,128> lifecycle;
    std::atomic<std::uint32_t> lifecycleNext{};
    std::atomic<std::uint32_t> next{},generation{},threadID{};
    std::atomic<bool> measuring{};
    std::array<std::uint64_t,25> channels{};
    std::array<std::uint8_t,16> outputSettings{};
    bool removed{},running{},suspendRequested{};
    std::uint32_t voices{},batches{},refused{};
    std::uint64_t deadline{},nextBatch{},restoreDeadline{},wakeDeadline{},startNs{},oldOutput{};
    D3QualificationState::CpuWindow cpuWindow;
    Json events=Json::array();
    void Event(const char* name,Json detail=Json::object()) { detail["name"]=name;detail["atNs"]=D3AudioProbe::NowNs();events.push_back(detail); }
    void Render(void*,std::uint32_t location,void*) noexcept {
        thread_local std::uint64_t begin{};
        thread_local std::uint32_t gen{};
        if(location==16) threadID.store(GetCurrentThreadId());
        if(!measuring.load(std::memory_order_relaxed)) return;
        const auto now=D3AudioProbe::NowNs();
        if(location==2048 || location==4096) {
            const auto index=lifecycleNext.fetch_add(1);
            if(index<lifecycle.size()) {
                auto& e=lifecycle[index];e.at=now;e.location=location;e.generation=generation.load();e.thread=GetCurrentThreadId();
                e.ready.store(true,std::memory_order_release);
            }
        } else if(location==16) {begin=now;gen=generation.load();}
        else if(location==32 && begin && gen==generation.load()) {
            const auto index=next.fetch_add(1);
            if(index<frames.size()) {
                auto& f=frames[index];f.begin=begin;f.end=now;f.generation=gen;f.thread=GetCurrentThreadId();
                f.ready.store(true,std::memory_order_release);
            }
            begin=0;
        }
    }
    std::optional<D3QualificationState::CpuReading> CPU() {
        const auto id=threadID.load();
        if(!id) return std::nullopt;
        const auto thread=OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,id);
        if(!thread) return std::nullopt;
        FILETIME create{},exit{},kernel{},user{};
        const auto ok=GetThreadTimes(thread,&create,&exit,&kernel,&user);CloseHandle(thread);
        if(!ok)return std::nullopt;
        const auto ticks=((std::uint64_t(kernel.dwHighDateTime)<<32)|kernel.dwLowDateTime)+
            ((std::uint64_t(user.dwHighDateTime)<<32)|user.dwLowDateTime);
        return D3QualificationState::CpuReading{id,D3AudioProbe::NowNs(),ticks};
    }
    void SampleCPU() {
        if(const auto reading=CPU()) cpuWindow.Observe(*reading);
    }
    template<class T,std::size_t N> T Bind(std::uint64_t id,std::uintptr_t rva,const std::array<std::uint8_t,N>& bytes) {
        const auto address=REL::ID(id).address();
        if(address!=REL::Offset(rva).address() || std::memcmp(reinterpret_cast<void*>(address),bytes.data(),N))
            throw std::runtime_error("D3 qualification binding rejected by address/byte gate");
        return reinterpret_cast<T>(address);
    }
    void Init() {
        if(registerCallback)return;
        const auto b=D3OutputBuffers::Snapshot();
        if(!b.value("installed",false) || b.value("result",0u)!=1)throw std::runtime_error("D3 qualification requires verified private buffer initialization");
        // Static native caller + dispatcher proof on 1.16.244, captured in OSF RE
        // systems.audio_wwise. Six register arguments; output settings copied opaque.
        const auto candidateRegister=Bind<Register>(150403,0x2CD5F50,std::array<std::uint8_t,13>{0x40,0x53,0x56,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x40});
        outputID=Bind<OutputID>(150376,0x2CD4790,std::array<std::uint8_t,15>{0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20});
        removeOutput=Bind<Remove>(150406,0x2CD6190,std::array<std::uint8_t,9>{0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9});
        addOutput=Bind<Add>(150350,0x2CD3850,std::array<std::uint8_t,15>{0x48,0x89,0x74,0x24,0x20,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x20});
        // Native game calls pass one bool to Suspend and no arguments to Wakeup.
        // Both enqueue message 0x36; 0x800/0x1000 callbacks report later transitions.
        suspendOutput=Bind<Suspend>(150431,0x2CD7230,std::array<std::uint8_t,14>{0x44,0x0F,0xB6,0xC1,0x45,0x33,0xC9,0x48,0x8B,0x0D,0x1A,0x96,0x08,0x03});
        wakeOutput=Bind<Wake>(150440,0x2CD7B60,std::array<std::uint8_t,15>{0x48,0x8B,0x0D,0xF1,0x8C,0x08,0x03,0x45,0x33,0xC9,0x41,0xB0,0x01,0x33,0xD2});
        const auto result=candidateRegister(Render,16|32|2048|4096,nullptr,0,0,0);
        if(result!=1)throw std::runtime_error("D3 render callback registration failed");
        // Publish initialized state only after every binding and registration passed.
        // A rejected later byte gate must not let a subsequent command skip Init.
        registerCallback=candidateRegister;
        std::memcpy(outputSettings.data(),reinterpret_cast<void*>(REL::Offset(0x5D608A0).address()),16);
        Event("registered",{{"result",result},{"outputSettings",outputSettings}});
    }
    void Restore() {
        if(!removed)return;
        std::uint64_t id{};
        const auto result=addOutput(outputSettings.data(),&id,nullptr,0);
        Event("output-add",{{"result",result},{"returnedID",id},{"oldID",oldOutput}});
        if(result==1)removed=false;
    }
    void Close() {
        running=false;
        const auto* services=API::RequestEngineAPI();
        D3QualificationState::CloseChannels(channels,[services](std::uint64_t channel) { services->CloseAudioChannel(channel); });
        Event("channels-closed");
    }
    void WakeOutput() {
        if(!suspendRequested)return;
        const auto at=D3AudioProbe::NowNs();
        const auto result=wakeOutput();
        Event("output-wake",{{"requestNs",at},{"result",result}});
        if(result==1)suspendRequested=false;
    }
    Json Snapshot() {
        Json rows=Json::array();const auto gen=generation.load(),count=next.load();
        for(std::size_t i=0;i<std::min<std::size_t>(count,frames.size());++i) {
            const auto& f=frames[i];
            if(f.ready.load(std::memory_order_acquire)&&f.generation==gen)rows.push_back({f.begin,f.end,f.thread});
        }
        Json transitions=Json::array();const auto lifecycleCount=lifecycleNext.load();
        for(std::size_t i=0;i<std::min<std::size_t>(lifecycleCount,lifecycle.size());++i) {
            const auto& e=lifecycle[i];
            if(e.ready.load(std::memory_order_acquire)&&e.generation==gen)
                transitions.push_back({{"atNs",e.at},{"location",e.location},{"thread",e.thread}});
        }
        if(measuring.load()) SampleCPU();
        const auto& cpu=cpuWindow.Snapshot();
        Json result={{"running",running},{"removed",removed},{"voices",voices},{"batches",batches},{"refused",refused},
            {"startNs",startNs},{"snapshotNs",D3AudioProbe::NowNs()},
            {"cpuAvailable",cpu.has_value()},{"cpuStart100ns",nullptr},{"cpuNow100ns",nullptr},
            {"cpuStartNs",nullptr},{"cpuSampleNs",nullptr},{"cpuThreadID",nullptr},{"threadID",threadID.load()},
            {"suspendRequested",suspendRequested},{"lifecycle",transitions},{"lifecycleOverflow",lifecycleCount>lifecycle.size()},
            {"outputID",outputID?outputID(0,0):0},{"events",events},{"renderFrames",rows},{"overflow",count>frames.size()}};
        if(cpu) {
            result["cpuStart100ns"]=cpu->first.ticks100ns;result["cpuNow100ns"]=cpu->last.ticks100ns;
            result["cpuStartNs"]=cpu->first.atNs;result["cpuSampleNs"]=cpu->last.atNs;
            result["cpuThreadID"]=cpu->last.thread;
        }
        return result;
    }
}
void Tick() {
    const auto now=D3AudioProbe::NowNs();
    if(measuring.load() && !cpuWindow.Started()) SampleCPU();
    if(removed && now>=restoreDeadline)Restore();
    if(suspendRequested && now>=wakeDeadline)WakeOutput();
    if(!running)return;
    if(now>=deadline){Close();return;}
    if(now<nextBatch)return;
    // No catch-up burst after a delayed game frame. 24 voices maximum, 1.2 s cadence.
    nextBatch=now+1200000000ULL;++batches;
    const char* paths[]={"OSF/D3/qualification/voice.wem","OSF/D3/qualification/partner.wem",
        "OSF/D3/qualification/voice.wav","OSF/D3/qualification/partner.wav"};
    for(std::uint32_t i=0;i<voices;++i)
        if(!API::RequestEngineAPI()->PlayAudioChannel(channels[i],paths[(i+batches)%4]))++refused;
}
Json Command(const Json& args) {
    Init();const auto action=args.at("action").get<std::string>();
    if(action=="begin") {
        if(running||removed||suspendRequested)throw std::runtime_error("D3 qualification already running");
        Close();measuring=false;++generation;startNs=D3AudioProbe::NowNs();cpuWindow.Reset();SampleCPU();measuring=true;
        batches=refused=0;voices=0;events=Json::array();Event("begin");
    } else if(action=="post") {
        const auto slot=args.value("slot",0u);const auto media=args.at("media").get<std::string>();
        if(slot>=channels.size() || (media!="voice.wem"&&media!="partner.wem"&&media!="voice.wav"&&media!="partner.wav"&&media!="sentinel.wav"))
            throw std::runtime_error("D3 qualification media/slot outside allowlist");
        if(!channels[slot])channels[slot]=API::RequestEngineAPI()->CreateAudioChannel();
        const auto path="OSF/D3/qualification/"+media;
        const auto accepted=API::RequestEngineAPI()->PlayAudioChannel(channels[slot],path.c_str());
        Event("post",{{"media",media},{"slot",slot},{"accepted",accepted}});
    } else if(action=="load") {
        const auto requestedVoices=args.at("voices").get<std::uint32_t>();const auto seconds=args.value("seconds",20u);
        if(running||requestedVoices<1||requestedVoices>24||seconds<1||seconds>60)throw std::runtime_error("D3 load outside bounded limits");
        const auto* services=API::RequestEngineAPI();
        if(!D3QualificationState::ReplaceChannels(std::span(channels).first(requestedVoices),
            [services] { return services->CreateAudioChannel(); },
            [services](std::uint64_t channel) { services->CloseAudioChannel(channel); }))
            throw std::runtime_error("D3 load channel allocation refused");
        voices=requestedVoices;
        nextBatch=D3AudioProbe::NowNs();deadline=nextBatch+std::uint64_t(seconds)*1000000000;running=true;
        Event("load",{{"voices",voices},{"seconds",seconds}});
    } else if(action=="suspend") {
        if(suspendRequested||removed)throw std::runtime_error("D3 output transition already pending");
        const bool render=args.value("render",false);const auto at=D3AudioProbe::NowNs();
        const auto result=suspendOutput(render);
        if(result==1){suspendRequested=true;wakeDeadline=D3AudioProbe::NowNs()+3000000000ULL;}
        Event("output-suspend",{{"requestNs",at},{"render",render},{"result",result}});
    } else if(action=="wake")WakeOutput();
    else if(action=="remove") {
        if(removed)throw std::runtime_error("D3 output already removed");
        oldOutput=outputID(0,0);const auto result=removeOutput(oldOutput);
        if(result==1){removed=true;restoreDeadline=D3AudioProbe::NowNs()+3000000000ULL;}
        Event("output-remove",{{"result",result},{"id",oldOutput}});
    } else if(action=="restore")Restore();
    else if(action=="close")Close();
    else if(action=="end"){Close();WakeOutput();Restore();if(measuring.load())SampleCPU();measuring=false;Event("end");}
    else if(action!="status")throw std::runtime_error("Unknown D3 qualification action");
    return Snapshot();
}
}
#endif
