#pragma once
#ifdef OSF_ANIMATION_SMOKE
#include <nlohmann/json.hpp>
namespace OSF::Testing::D3WwiseTrace
{
    void Configure(bool enabled, std::uint32_t stopTransitionMs, std::uint32_t mixStride=1);
    void* Cookie(std::uint64_t sample);
    void Callback(std::uint32_t type, void* info) noexcept;
    void Posted(void* cookie, std::uint32_t playing);
    std::int32_t StopTransition(std::uint32_t playing);
    nlohmann::json Snapshot();
}
#endif
