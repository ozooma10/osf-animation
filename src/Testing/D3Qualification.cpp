#include "Testing/D3Qualification.h"
#ifdef OSF_ANIMATION_SMOKE
#include "Testing/D3AudioProbe.h"
#include "Testing/D3OutputBuffers.h"
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
    Register registerCallback{}; OutputID outputID{}; Remove removeOutput{}; Add addOutput{};
    struct Frame {std::atomic<bool> ready{}; std::uint64_t begin{},end{};std::uint32_t generation{},thread{};};
    std::array<Frame,32768> frames;
    std::atomic<std::uint32_t> next{},generation{},threadID{};
    std::atomic<bool> measuring{};
    std::array<std::uint64_t,25> channels{};
    std::array<std::uint8_t,16> outputSettings{};
    bool removed{},running{};
    std::uint32_t voices{},batches{},refused{};
    std::uint64_t deadline{},nextBatch{},restoreDeadline{},startNs{},cpuStart{},oldOutput{};
    Json events=Json::array();
    void Event(const char* name,Json detail=Json::object()) { detail["name"]=name;detail["atNs"]=D3AudioProbe::NowNs();events.push_back(detail); }
    void Render(void*,std::uint32_t location,void*) noexcept {
        thread_local std::uint64_t begin{};
        thread_local std::uint32_t gen{};
        if(!measuring.load(std::memory_order_relaxed)) return;
        const auto now=D3AudioProbe::NowNs();
        if(location==16) {begin=now;gen=generation.load();threadID.store(GetCurrentThreadId());}
        else if(location==32 && begin && gen==generation.load()) {
            const auto index=next.fetch_add(1);
            if(index<frames.size()) {
                auto& f=frames[index];f.begin=begin;f.end=now;f.generation=gen;f.thread=GetCurrentThreadId();
                f.ready.store(true,std::memory_order_release);
            }
            begin=0;
        }
    }
    std::uint64_t CPU() {
        const auto thread=OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,threadID.load());
        if(!thread) return 0;
        FILETIME create{},exit{},kernel{},user{};
        const auto ok=GetThreadTimes(thread,&create,&exit,&kernel,&user);CloseHandle(thread);
        if(!ok)return 0;
        return ((std::uint64_t(kernel.dwHighDateTime)<<32)|kernel.dwLowDateTime)+((std::uint64_t(user.dwHighDateTime)<<32)|user.dwLowDateTime);
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
        registerCallback=Bind<Register>(150403,0x2CD5F50,std::array<std::uint8_t,13>{0x40,0x53,0x56,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x40});
        outputID=Bind<OutputID>(150376,0x2CD4790,std::array<std::uint8_t,15>{0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20});
        removeOutput=Bind<Remove>(150406,0x2CD6190,std::array<std::uint8_t,9>{0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9});
        addOutput=Bind<Add>(150350,0x2CD3850,std::array<std::uint8_t,15>{0x48,0x89,0x74,0x24,0x20,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x20});
        const auto result=registerCallback(Render,16|32,nullptr,0,0,0);
        if(result!=1){registerCallback=nullptr;throw std::runtime_error("D3 render callback registration failed");}
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
        for(auto& channel:channels){if(channel)API::RequestEngineAPI()->CloseAudioChannel(channel);channel=0;}
        Event("channels-closed");
    }
    Json Snapshot() {
        Json rows=Json::array();const auto gen=generation.load(),count=next.load();
        for(std::size_t i=0;i<std::min<std::size_t>(count,frames.size());++i) {
            const auto& f=frames[i];
            if(f.ready.load(std::memory_order_acquire)&&f.generation==gen)rows.push_back({f.begin,f.end,f.thread});
        }
        const auto now=D3AudioProbe::NowNs(),cpu=CPU();
        return {{"running",running},{"removed",removed},{"voices",voices},{"batches",batches},{"refused",refused},
            {"startNs",startNs},{"snapshotNs",now},{"cpuStart100ns",cpuStart},{"cpuNow100ns",cpu},{"threadID",threadID.load()},
            {"outputID",outputID?outputID(0,0):0},{"events",events},{"renderFrames",rows},{"overflow",count>frames.size()}};
    }
}
void Tick() {
    const auto now=D3AudioProbe::NowNs();
    if(removed && now>=restoreDeadline)Restore();
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
        if(running||removed)throw std::runtime_error("D3 qualification already running");
        Close();measuring=false;++generation;startNs=D3AudioProbe::NowNs();cpuStart=CPU();measuring=true;
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
        voices=args.at("voices").get<std::uint32_t>();const auto seconds=args.value("seconds",20u);
        if(running||voices<1||voices>24||seconds<1||seconds>60)throw std::runtime_error("D3 load outside bounded limits");
        for(std::uint32_t i=0;i<voices;++i)channels[i]=API::RequestEngineAPI()->CreateAudioChannel();
        nextBatch=D3AudioProbe::NowNs();deadline=nextBatch+std::uint64_t(seconds)*1000000000;running=true;
        Event("load",{{"voices",voices},{"seconds",seconds}});
    } else if(action=="remove") {
        if(removed)throw std::runtime_error("D3 output already removed");
        oldOutput=outputID(0,0);const auto result=removeOutput(oldOutput);
        if(result==1){removed=true;restoreDeadline=D3AudioProbe::NowNs()+3000000000ULL;}
        Event("output-remove",{{"result",result},{"id",oldOutput}});
    } else if(action=="restore")Restore();
    else if(action=="close")Close();
    else if(action=="end"){Close();Restore();measuring=false;Event("end");}
    else if(action!="status")throw std::runtime_error("Unknown D3 qualification action");
    return Snapshot();
}
}
#endif
