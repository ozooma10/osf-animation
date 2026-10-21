#pragma once
#ifdef OSF_ANIMATION_SMOKE
#include <nlohmann/json.hpp>
namespace OSF::Testing::D3Qualification {
    void Tick();
    nlohmann::json Command(const nlohmann::json& args);
}
#endif
