#pragma once
#include <chrono>
#include <cstdint>

namespace OSF::Util {
    // Process-local monotonic nanoseconds. Exposed through services for cross-plugin deadlines.
    inline std::uint64_t MonotonicNs() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
}
