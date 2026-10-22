#include "Scene/SceneRuntime.h"
#include "Animation/GraphManager.h"
#include "Profiling/Profile.h"
#include "UI/FadeService.h"
#include "Util/MonotonicClock.h"
#include "Util/NativeMainThreadQueue.h"

namespace OSF::Scene {
    void SceneRuntime::ReleaseCover(const std::shared_ptr<PreparedTransition>& p) {
        if (p->coverReleased) return;
        p->coverReleased = true;
        UI::FadeService::GetSingleton().EndTransition(p->coverToken);
        bool resume{};
        {
            std::lock_guard lock{_lock};
            if (auto* s = Resolve(p->context.scene); s && s->cover == p) {
                s->cover.reset();
                if (!s->preparation) s->transitioning = false;
                resume = p->committed && !p->reset && s->playbackId == p->published;
            }
        }
        if (resume) Animation::GraphManager::GetSingleton().HoldForPreparation(p->cast.front(),p->published,false);
    }
    void SceneRuntime::PollCover(const std::shared_ptr<PreparedTransition>& p) {
        Util::NativeMainThreadQueue::Post(OSF_PROFILE_TASK("Scene.PollCover", [this,p] {
            if (p->coverReleased) return;
            bool live{};
            {
                std::lock_guard lock{_lock};
                const auto* s = Resolve(p->context.scene);
                live = s && s->cover == p;
            }
            if (p->reset || !live) { ReleaseCover(p); return; }
            const auto now = Util::MonotonicNs() / 1000000;
            auto& fade = UI::FadeService::GetSingleton();
            const bool expired = p->coverState.Expired(now) || !fade.OwnsTransition(p->coverToken);
            if (p->cancelled || expired) {
                if (expired) { p->failureReason = "transition-cover-timeout"; p->interrupted = true; }
                p->cancelled = true;
                if (!p->committed && p->context.attempt)
                    _preparers.Cancel(p->context.attempt, expired ? Preparation::Outcome::TimedOut : Preparation::Outcome::Cancelled);
                ReleaseCover(p);
                if (p->committed) Stop(p->context.scene);
                // A not-yet-started preparation observes cancellation in its already queued callback.
                return;
            }
            if (p->coverState.CanReveal(Animation::GraphManager::GetSingleton().SceneBlendComplete(p->cast,p->published))) {
                if (fade.RevealTransition(p->coverToken)) p->coverState.Revealing(now);
            }
            if (p->coverState.Finished(now)) { ReleaseCover(p); return; }
            PollCover(p);
        }));
    }
}
