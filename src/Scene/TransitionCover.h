#pragma once
#include <cstdint>

namespace OSF::Scene {
    // Wall-clock deadlines, deliberately independent of playback speed and clip duration.
    struct TransitionCover {
        enum class Phase { Closing, Preparing, Blending, Revealing };
        std::uint64_t started{}, revealUntil{};
        Phase phase{Phase::Closing};
        bool Expired(std::uint64_t now) const { return phase != Phase::Revealing && now >= started + 5000; }
        bool PreparationReady(std::uint64_t now) {
            if (Expired(now)) return false;
            if (phase == Phase::Closing && now >= started + 300) phase = Phase::Preparing;
            return phase == Phase::Preparing;
        }
        void Committed() { phase = Phase::Blending; }
        bool CanReveal(bool allPosesStamped) const { return phase == Phase::Blending && allPosesStamped; }
        void Revealing(std::uint64_t now) { phase = Phase::Revealing; revealUntil = now + 500; }
        bool Finished(std::uint64_t now) const { return phase == Phase::Revealing && now >= revealUntil; }
        const char* Name() const {
            switch (phase) {
            case Phase::Closing: return "closing";
            case Phase::Preparing: return "preparing";
            case Phase::Blending: return "blending";
            default: return "revealing";
            }
        }
    };
}
