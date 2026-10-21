#include "Testing/D3WwiseTrace.h"
#ifdef OSF_ANIMATION_SMOKE
#include "Testing/D3AudioProbe.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>

namespace OSF::Testing::D3WwiseTrace
{
    namespace
    {
        // Process-lifetime, append-only storage: old callbacks cannot write into a
        // new trial's records. The audio callback never locks, allocates or logs.
        struct Record {
            std::atomic<bool> ready{false};
            std::uint64_t cookie{}, ns{}, object{};
            std::uint32_t type{}, playing{}, event{};
            float duration{}, estimated{};
        };
        std::array<Record,32768> records;
        std::atomic<std::uint32_t> next{}, generation{}, transition{};
        std::atomic<bool> enabled{};
        static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
        static_assert(std::atomic<bool>::is_always_lock_free);
        // Wwise x64 AkEventCallbackInfo prefix. Live-checked on 1.16.244:
        // 92 matching IDs/objects/events and exact known media durations;
        // canonical proof: OSF RE systems.audio_wwise, D3 output/stop note.
        // Read only copied scalars;
        // never retain a callback-info pointer after returning to the engine.
        struct EventInfo { void* cookie; std::uint64_t object; std::uint32_t playing, event; };
        static_assert(sizeof(EventInfo)==24 && offsetof(EventInfo,playing)==16);
        void Append(std::uint32_t type, const EventInfo& info, float duration=0, float estimated=0) noexcept
        {
            const auto ns=D3AudioProbe::NowNs();
            const auto index=next.fetch_add(1,std::memory_order_relaxed);
            if (index>=records.size()) return;
            auto& r=records[index];
            r.cookie=reinterpret_cast<std::uintptr_t>(info.cookie); r.ns=ns;
            r.object=info.object; r.type=type; r.playing=info.playing; r.event=info.event;
            r.duration=duration; r.estimated=estimated;
            r.ready.store(true,std::memory_order_release);
        }
    }
    void Configure(bool enable, std::uint32_t stopTransitionMs)
    {
        enabled.store(false);
        if (stopTransitionMs>500) throw std::runtime_error("D3 stop transition exceeds 500 ms");
        ++generation; transition.store(stopTransitionMs); enabled.store(enable);
    }
    void* Cookie(std::uint64_t sample)
    {
        if (!enabled.load() || !sample || sample>4096) return nullptr;
        return reinterpret_cast<void*>((std::uint64_t(generation.load())<<32)|sample);
    }
    void Callback(std::uint32_t type, void* data) noexcept
    {
        if (!data || (type!=1 && type!=8 && type!=16)) return;
        EventInfo info; std::memcpy(&info,data,sizeof(info));
        float duration{}, estimated{};
        if (type==8) {
            std::memcpy(&duration,static_cast<const char*>(data)+24,sizeof(float));
            std::memcpy(&estimated,static_cast<const char*>(data)+28,sizeof(float));
        }
        Append(type,info,duration,estimated);
    }
    void Posted(void* cookie, std::uint32_t playing)
    {
        if (cookie && playing) Append(0,{cookie,0,playing,0});
    }
    std::int32_t StopTransition(std::uint32_t playing)
    {
        if (!enabled.load()) return 0;
        const auto gen=generation.load();
        const auto count=std::min<std::size_t>(next.load(),records.size());
        for (std::size_t i=0;i<count;++i) {
            const auto& r=records[i];
            if (r.ready.load(std::memory_order_acquire) && r.type==0 && r.playing==playing && (r.cookie>>32)==gen) {
                Append(256,{reinterpret_cast<void*>(r.cookie),0,playing,0});
                return static_cast<std::int32_t>(transition.load());
            }
        }
        return 0;
    }
    nlohmann::json Snapshot()
    {
        auto entries=nlohmann::json::array();
        const auto gen=generation.load(), count=next.load();
        for (std::size_t i=0;i<std::min<std::size_t>(count,records.size());++i) {
            const auto& r=records[i];
            if (!r.ready.load(std::memory_order_acquire) || (r.cookie>>32)!=gen) continue;
            entries.push_back({{"sample",r.cookie&0xFFFFFFFF},{"atNs",r.ns},{"type",r.type},
                {"playingID",r.playing},{"eventID",r.event},{"gameObject",r.object},
                {"durationMs",r.duration},{"estimatedMs",r.estimated}});
        }
        return {{"enabled",enabled.load()},{"generation",gen},{"stopTransitionMs",transition.load()},
            {"overflow",count>records.size()},{"records",entries}};
    }
}
#endif
