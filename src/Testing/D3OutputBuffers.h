#pragma once
#ifdef OSF_ANIMATION_SMOKE
#include <nlohmann/json.hpp>
namespace OSF::Testing::D3OutputBuffers
{
    void Install(const REL::Version& runtime);
    nlohmann::json Snapshot();
}
#endif
