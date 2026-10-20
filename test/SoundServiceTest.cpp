#include "Audio/SoundService.h"
#include "Audio/WwiseBackend.h"
#include "Check.h"
#include "Util/MonotonicClock.h"
#include <condition_variable>
#include <future>
#include <thread>
#include <unordered_set>

namespace
{
    using OSF::Test::Check;
    std::mutex fakeLock;
    std::condition_variable fakeCV;
    bool holdPrepare{}, prepareEntered{}, holdPost{}, postEntered{}, rejectNext{};
    std::size_t prepareCount{};
    std::vector<std::wstring> preparedPaths;
    std::uint32_t nextVoice{1};
    std::vector<std::uint32_t> posted, stopped;
    std::uint32_t Post()
    {
        std::unique_lock l(fakeLock);
        postEntered = true; fakeCV.notify_all();
        if (!fakeCV.wait_for(l, std::chrono::seconds(5), [] { return !holdPost; }))
            Check(false, "fake submission gate expired");
        if (rejectNext) { rejectNext = false; return 0; }
        posted.push_back(nextVoice);
        return nextVoice++;
    }
    void Arm(bool submission = false)
    {
        std::lock_guard l(fakeLock);
        prepareEntered = postEntered = false;
        prepareCount = 0; preparedPaths.clear();
        holdPrepare = !submission; holdPost = submission;
    }
    void Await(bool submission = false)
    {
        std::unique_lock l(fakeLock);
        Check(fakeCV.wait_for(l, std::chrono::seconds(5), [=] { return submission ? postEntered : prepareEntered; }),
            "audio job reached deterministic gate");
    }
    void Release()
    {
        { std::lock_guard l(fakeLock); holdPrepare = holdPost = false; }
        fakeCV.notify_all();
    }
    std::size_t Posts() { std::lock_guard l(fakeLock); return posted.size(); }
    void RejectNext() { std::lock_guard l(fakeLock); rejectNext = true; }
    std::uint32_t Last() { std::lock_guard l(fakeLock); return posted.back(); }
    bool Stopped(std::uint32_t voice)
    {
        std::lock_guard l(fakeLock);
        return std::ranges::find(stopped, voice) != stopped.end();
    }
}

// Link the production service/worker to a deterministic backend; never call the game.
namespace OSF::Audio::Wwise
{
    std::optional<std::uint32_t> ParseEventSpec(std::string_view value)
    { return value == "event:test" ? std::optional<std::uint32_t>(1) : std::nullopt; }
    bool IsWwiseExternalSource(std::string_view value) { return value.ends_with(".wav"); }
    PreparedMedia PrepareExternalFile(const std::wstring& path)
    {
        std::unique_lock l(fakeLock);
        prepareEntered = true; fakeCV.notify_all();
        ++prepareCount; preparedPaths.push_back(path); fakeCV.notify_all();
        if (!fakeCV.wait_for(l, std::chrono::seconds(5), [] { return !holdPrepare; }))
            Check(false, "fake preparation gate expired");
        if(path.ends_with(L"missing.wav")) return {};
        static std::byte byte;
        return {&byte, 1, RE::BGSAudio::AkCodecID::kPCM};
    }
    std::uint32_t PostPreparedFile(const PreparedMedia&, std::uint64_t) { return Post(); }
    std::uint32_t PostEvent(std::uint32_t, std::uint64_t) { return Post(); }
    bool StopVoice(std::uint32_t voice)
    {
        std::lock_guard l(fakeLock); stopped.push_back(voice); return true;
    }
}

void TestSoundService()
{
    auto& service = OSF::Audio::SoundService::GetSingleton();
    service.StopAll();
    const auto voice = service.CreateChannel(), contact = service.CreateChannel(), unrelated = service.CreateChannel();
    Check(voice && contact && unrelated && voice != contact && contact != unrelated, "independent opaque channels");
    Check(service.PlayChannel(voice, "event:test"), "voice starts"); const auto v = Last();
    Check(service.PlayChannel(contact, "event:test"), "contact starts"); const auto c = Last();
    Check(service.PlayChannel(unrelated, "event:test"), "other owner starts"); const auto u = Last();
    Check(!Stopped(v) && !Stopped(c), "contact and unrelated owner do not replace voice");
    Check(service.CloseChannel(voice) && Stopped(v) && !Stopped(c) && !Stopped(u), "closing voice preserves other channels");
    Check(!service.CloseChannel(voice) && !service.PlayChannel(voice, "event:test"), "retired handles cannot be reused");
    Check(!service.PlayChannel(contact, "../outside.wav") && !service.PlayChannel(contact, "C:/outside.wav") &&
        !service.PlayChannel(contact, "$pool") && !service.PlayChannel(0, "event:test"), "public channel validates specs and handles");

    RejectNext();
    Check(!service.PlayChannel(contact, "event:test") && !Stopped(c), "rejected event preserves the current voice");
    RejectNext();
    Check(service.PlayChannel(contact, "rejected.wav"), "file admission does not imply Wwise acceptance");
    Check(service.WaitIdleForTest() && !Stopped(c), "rejected file preserves the current voice");
    Arm(); Check(service.PlayChannel(contact, "held.wav"), "older replacement queued"); Await();
    RejectNext(); auto acceptedBefore = Posts();
    Check(!service.PlayChannel(contact, "event:test"), "newer event rejected"); Release();
    Check(service.WaitIdleForTest() && Posts() == acceptedBefore + 1 && Stopped(c),
        "failed newer request does not invalidate an older pending replacement");

    // Cancel a decoded/queued request before submission, then prove the worker finished without posting.
    auto channel = service.CreateChannel(); auto before = Posts();
    Arm(); Check(service.PlayChannel(channel, "held.wav"), "file queued"); Await();
    Check(service.CloseChannel(channel), "close does not wait for decoding"); Release();
    Check(service.WaitIdleForTest() && Posts() == before, "closed scope never reaches backend submission");

    channel = service.CreateChannel(); before = Posts();
    Arm(); Check(service.PlayChannel(channel, "held.wav"), "world-reset file queued"); Await();
    service.StopAll(); Release();
    Check(service.WaitIdleForTest() && Posts() == before, "world reset during preparation prevents submission");
    Check(!service.PlayChannel(channel, "event:test"), "world reset revokes handles");

    // A newer published request prevents a late older completion on the same channel.
    channel = service.CreateChannel(); Arm();
    Check(service.PlayChannel(channel, "held.wav"), "older file queued"); Await();
    Check(service.PlayChannel(channel, "event:test"), "newer event publishes"); before = Posts();
    const auto newer = Last(); Release();
    Check(service.WaitIdleForTest() && Posts() == before && !Stopped(newer), "obsolete completion cannot post over newer voice");
    service.CloseChannel(channel);

    // If posting wins the lock, Close waits for it and stops that ID before returning.
    channel = service.CreateChannel(); Arm(true);
    auto play = std::async(std::launch::async, [&] { return service.PlayChannel(channel, "event:test"); });
    Await(true);
    auto close = std::async(std::launch::async, [&] { return service.CloseChannel(channel); });
    Check(close.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout, "close serializes with an in-progress post");
    Release(); Check(play.get() && close.get() && Stopped(Last()), "post winner is stopped before close returns");

    // Legacy slots keep their authored-tail policy but gain the world/sequence commit check.
    Arm(); before = Posts(); service.Play(77, "held.wav"); Await();
    service.StopAll(); Release();
    Check(service.WaitIdleForTest() && Posts() == before, "legacy slot cannot post after world reset");
    Arm(); service.Play(77, "held.wav"); Await(); service.Play(77, "event:test"); before = Posts(); Release();
    Check(service.WaitIdleForTest() && Posts() == before, "legacy older completion cannot post over published replacement");
    service.StopAll();

    // Authored playback can join a consumer-owned channel without affecting other slots.
    auto bound = service.CreateChannel();
    auto competitor = service.CreateChannel();
    Check(service.BindSlot(91,bound) && !service.BindSlot(91,competitor), "actor sound binding cannot steal another owner");
    service.Play(91,"event:test"); const auto authored=Last();
    Check(service.PlayChannel(bound,"event:test") && Stopped(authored), "generated and authored sound share one channel");
    Arm(); before=Posts(); service.Play(91,"held.wav"); Await();
    service.CloseChannel(bound); Release();
    Check(service.WaitIdleForTest() && Posts()==before, "closing scope cancels authored queued work");
    Check(service.BindSlot(91,competitor), "closing releases actor binding for a fresh scope");
    service.StopAll();

    // Both workers are executing. Hundreds of replacements must occupy only one waiting
    // slot per opted channel, leaving admission capacity for an unrelated default owner.
    const auto latest = service.CreateChannel(true), latestPeer = service.CreateChannel(true);
    const auto defaultOwner = service.CreateChannel();
    Arm(); before=Posts();
    Check(service.PlayChannel(latest,"running-a.wav") && service.PlayChannel(latestPeer,"running-b.wav"),"coalescing workers admitted");
    {
        std::unique_lock l(fakeLock);
        Check(fakeCV.wait_for(l,std::chrono::seconds(5),[]{return prepareCount==2;}),"both workers held before coalescing");
    }
    bool allAccepted=true;
    for(int i=0;i<400;++i) allAccepted &= service.PlayChannel(latest,"queued-"+std::to_string(i)+".wav");
    Check(allAccepted && service.PlayChannel(latestPeer,"peer-latest.wav") &&
        service.PlayChannel(defaultOwner,"unrelated.wav"),"coalescing occurs before global capacity rejection");
    Check(!service.PlayChannel(latest,"../invalid.wav"),"invalid replacement does not supersede latest pending");
    Release();
    Check(service.WaitIdleForTest() && Posts()==before+3,"only newest per channel and unrelated owner post after preparation");
    {
        std::lock_guard l(fakeLock);
        Check(preparedPaths.size()==5 && std::ranges::find(preparedPaths,std::wstring(L"Data\\queued-399.wav"))!=preparedPaths.end(),
            "superseded waiting jobs never prepare media");
    }
    service.StopAll();

    // Expiry is independent of supersession, includes pre-admission age, and preserves
    // the already playing voice. The default owner has no implicit expiry.
    const auto expiring=service.CreateChannel(true,30);
    Check(service.PlayChannel(expiring,"event:test"),"expiring channel starts"); const auto preserved=Last();
    const auto old=OSF::Util::MonotonicNs()-100000000;
    Check(!service.PlayChannel(expiring,"old.wav",old) && !service.PlayChannel(expiring,"event:test",old) && !Stopped(preserved),
        "expired file and event admissions preserve current voice");
    Check(!service.PlayChannel(expiring,"future.wav",OSF::Util::MonotonicNs()+1000000000),"future source timestamp rejected");
    Arm(); before=Posts(); Check(service.PlayChannel(expiring,"expires-in-prepare.wav"),"fresh deadline admitted"); Await();
    std::this_thread::sleep_for(std::chrono::milliseconds(45)); Release();
    Check(service.WaitIdleForTest() && Posts()==before && !Stopped(preserved),"expiry after preparation suppresses submission without a newer request");
    const auto timeless=service.CreateChannel();
    Check(service.PlayChannel(timeless,"old-default.wav",old) && service.WaitIdleForTest() && Posts()==before+1,
        "default channels do not acquire an implicit age limit");
    service.CloseChannel(expiring); service.CloseChannel(timeless);

    const auto failedReplacement=service.CreateChannel(true);
    Check(service.PlayChannel(failedReplacement,"event:test"),"opted channel has a current voice"); const auto stillPlaying=Last();
    Arm(); before=Posts(); Check(service.PlayChannel(failedReplacement,"held.wav"),"opted older request prepares"); Await();
    Check(service.PlayChannel(failedReplacement,"missing.wav"),"newer missing media is admitted before preparation");
    Release();
    Check(service.WaitIdleForTest() && Posts()==before && !Stopped(stillPlaying),
        "failed opted replacement preserves playing voice without reviving superseded work");
    service.CloseChannel(failedReplacement);

    // Close/world reset still fence opted channels while both waiting and executing.
    for(bool world : {false,true}) {
        const auto owner=service.CreateChannel(true,1000);
        Arm(); before=Posts(); Check(service.PlayChannel(owner,"held.wav"),"opted cancellation request queued"); Await();
        for(int i=0;i<10;++i) service.PlayChannel(owner,"waiting.wav");
        if(world) service.StopAll(); else service.CloseChannel(owner);
        Release(); Check(service.WaitIdleForTest() && Posts()==before,"opted cancellation drains without late posts");
    }

    std::vector<std::uint64_t> handles;
    for (std::size_t i = 0; i < 4096; ++i) handles.push_back(service.CreateChannel());
    Check(handles.back() != 0 && service.CreateChannel() == 0, "channel ownership is bounded");
    const auto retired = handles.back(); service.CloseChannel(retired);
    const auto fresh = service.CreateChannel();
    Check(fresh && fresh != retired, "retired IDs are not recycled");
    service.StopAll();
}
