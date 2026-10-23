#include "Audio/WwiseBackend.h"

#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace OSF::Audio::Wwise
{
    namespace
    {
        // Raw AK entry points, traced from the game's own registration/teardown.
        // 1.16.244: RegisterGameObj = 150401, UnregisterGameObj = 150436.
        // Do NOT use the historical CommonLib StopAll binding at 150401: it registers.
        using ObjectFn = std::uint32_t (*)(std::uint64_t);
        ObjectFn registerObject{}, unregisterObject{};
        bool supported{};
        constexpr std::size_t kMaxEmitters = 256;
        constexpr std::uint64_t kNamespace = 0x4F53464100000000ull;
        std::uint32_t nextObject = 1; // Never reused, including across world replacement.
        struct Emitter {
            std::uint32_t actor; RE::Actor* identity;
#ifdef OSF_ANIMATION_SMOKE
            RE::BGSAudio::AkSoundPosition position{}; std::uint64_t updates{};
#endif
        };
        std::unordered_map<std::uint64_t, Emitter> emitters;

        // Called only on the verified frame drain. Identity is compared, never dereferenced;
        // form lookup resolves current lifetime and the loaded-data read lock verifies 3D.
        bool Position(RE::Actor* actor, RE::BGSAudio::AkSoundPosition& out)
        {
            if (!actor || !actor->parentCell) return false;
            const auto loaded = actor->loadedData.LockRead();
            if (!*loaded || !(*loaded)->data3D) return false;
            const auto p = actor->data.location;
            const auto heading = actor->data.angle.z;
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(heading)) return false;
            // Native converter 0x140F195E0: game XYZ -> Wwise XZY, without a scale.
            out = {{-std::sin(heading), 0, std::cos(heading)}, {0, 1, 0}, {p.x, p.z, p.y}};
            return true;
        }
    }

    void InitializeRoleEmitters(const REL::Version& runtime)
    {
        if (runtime != REL::Version{1,16,244,0}) return;
        constexpr std::array<std::uint8_t, 15> objectBytes{
            0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x83,0xF9,0xE0,0x72,0x0B};
        constexpr std::array<std::uint8_t, 16> positionBytes{
            0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x30,0x48,0x8B,0xDA,0x48,0x8B,0xF9};
        const auto reg = REL::ID(150401).address(), unreg = REL::ID(150436).address();
        const auto pos = REL::ID(150420).address();
        if (reg != REL::Offset(0x2CD5D20).address() || unreg != REL::Offset(0x2CD7800).address() ||
            pos != REL::Offset(0x2CD6BA0).address() ||
            std::memcmp(reinterpret_cast<void*>(reg),objectBytes.data(),objectBytes.size()) ||
            std::memcmp(reinterpret_cast<void*>(unreg),objectBytes.data(),objectBytes.size()) ||
            std::memcmp(reinterpret_cast<void*>(pos),positionBytes.data(),positionBytes.size())) {
            REX::WARN("[Audio] role emitter native entry-point verification failed");
            return;
        }
        registerObject = reinterpret_cast<ObjectFn>(reg);
        unregisterObject = reinterpret_cast<ObjectFn>(unreg);
        supported = true;
    }

    std::uint64_t PrepareRoleEmitter(RE::Actor* actor)
    {
        RE::BGSAudio::AkSoundPosition position{};
        if (!supported || !Position(actor,position)) return 0;
        for (const auto& [id, emitter] : emitters) {
            if (emitter.actor == actor->formID && emitter.identity == actor)
                return RE::BGSAudio::AkSoundEngine::SetPosition(id,position) == 1 ? id : 0;
        }
        if (!nextObject || emitters.size() >= kMaxEmitters) return 0;
        const auto id = kNamespace | nextObject++;
        if (registerObject(id) != 1) return 0;
        if (RE::BGSAudio::AkSoundEngine::SetPosition(id,position) != 1) {
            unregisterObject(id);
            return 0;
        }
        emitters.emplace(id,Emitter{actor->formID,actor});
#ifdef OSF_ANIMATION_SMOKE
        emitters.at(id).position = position; emitters.at(id).updates = 1;
#endif
        REX::DEBUG("[Audio] registered role emitter {:#x} for actor {:08X}",id,actor->formID);
        return id;
    }

    bool RoleEmitterCurrent(std::uint64_t id) { return !id || emitters.contains(id); }

    std::vector<std::uint64_t> TickRoleEmitters()
    {
        std::vector<std::uint64_t> retired;
        for (auto it = emitters.begin(); it != emitters.end();) {
            auto* form = RE::TESForm::LookupByID(it->second.actor);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            RE::BGSAudio::AkSoundPosition position{};
            if (actor != it->second.identity || !Position(actor,position) ||
                RE::BGSAudio::AkSoundEngine::SetPosition(it->first,position) != 1) {
                retired.push_back(it->first);
                unregisterObject(it->first);
                it = emitters.erase(it);
            } else {
#ifdef OSF_ANIMATION_SMOKE
                it->second.position = position; ++it->second.updates;
#endif
                ++it;
            }
        }
        return retired;
    }

#ifdef OSF_ANIMATION_SMOKE
    std::vector<EmitterObservation> ObserveRoleEmitters()
    {
        std::vector<EmitterObservation> result;
        for (const auto& [id,e] : emitters) result.push_back({id,e.actor,e.position,e.updates});
        return result;
    }
#endif

    void ResetRoleEmitters()
    {
        for (const auto& [id, emitter] : emitters) { (void)emitter; unregisterObject(id); }
        emitters.clear();
    }
}
