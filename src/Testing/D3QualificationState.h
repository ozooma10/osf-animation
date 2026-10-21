#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace OSF::Testing::D3QualificationState
{
    struct CpuReading
    {
        std::uint32_t thread{};
        std::uint64_t atNs{}, ticks100ns{};
    };

    struct CpuInterval
    {
        CpuReading first, last;
    };

    // A missing thread reading is not a zero CPU counter. Start the measurement
    // window at the first available reading and retain its own wall-clock bounds.
    class CpuWindow
    {
    public:
        void Reset() { interval.reset(); }
        bool Started() const { return interval.has_value(); }
        void Observe(const CpuReading& reading)
        {
            if (!interval || reading.thread != interval->last.thread ||
                reading.ticks100ns < interval->last.ticks100ns || reading.atNs < interval->last.atNs) {
                interval = CpuInterval{reading, reading};
            } else {
                interval->last = reading;
            }
        }
        const std::optional<CpuInterval>& Snapshot() const { return interval; }

    private:
        std::optional<CpuInterval> interval;
    };

    template<class Close>
    void CloseChannels(std::span<std::uint64_t> channels, Close&& close)
    {
        for (auto& channel : channels) {
            if (const auto owned = std::exchange(channel, 0)) close(owned);
        }
    }

    // Retire every overwritten handle, including its pending work. Allocation
    // failure leaves the requested range empty and closes partial replacements.
    template<class Create, class Close>
    bool ReplaceChannels(std::span<std::uint64_t> channels, Create&& create, Close&& close)
    {
        CloseChannels(channels, close);
        for (auto& channel : channels) {
            channel = create();
            if (!channel) {
                CloseChannels(channels, close);
                return false;
            }
        }
        return true;
    }
}
