#pragma once

#ifdef OSF_ANIMATION_SMOKE
#include <nlohmann/json.hpp>

// Private instrumentation only. No consumer ABI and no production behavior changes.
namespace OSF::Testing::D3AudioProbe
{
    std::uint64_t NowNs();
    void FrameDrain();
    void BeginMark(std::int32_t scene, std::uint64_t playback, std::string_view node,
        std::uint8_t lane, std::string_view token, std::uint64_t crossed,
        std::uint64_t motionLateNs = 0, float inputDelta = 0);
    void EndMark();
    bool TryScopedSound(std::string_view spec);
    void VoiceStopped(std::uint32_t playing);
    std::uint64_t CaptureAudio(std::string_view path, std::uint64_t slot);
    void Point(std::uint64_t id, const char* name, std::uint64_t value = 0);
    void Checkpoint(std::uint64_t id, const char* name);
    std::uint64_t Current();
    struct AudioContext
    {
        explicit AudioContext(std::uint64_t id);
        ~AudioContext();
        std::uint64_t id, previous;
    };
    void WorldReset() noexcept;
    void Tick();
    nlohmann::json Command(const std::string& command, const nlohmann::json& args);
}
#endif
