#include "Testing/D3OutputBuffers.h"
#ifdef OSF_ANIMATION_SMOKE
#include "REL/THook.h"
#include <array>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>

namespace OSF::Testing::D3OutputBuffers
{
    namespace
    {
        using Json=nlohmann::json;
        std::mutex mutex;
        std::optional<REL::THook<std::uint32_t(void*,void*)>> hook;
        std::string mode;
        Json state={{"enabled",false},{"installed",false},{"calls",0}};
        template<class T> T Read(const void* p,std::size_t offset) {
            T value; std::memcpy(&value,static_cast<const char*>(p)+offset,sizeof(value)); return value;
        }
        template<class T> void Write(void* p,std::size_t offset,T value) {
            std::memcpy(static_cast<char*>(p)+offset,&value,sizeof(value));
        }
        Json Fields(const void* init,const void* platform) {
            return {{"samplesPerFrame",Read<std::uint32_t>(init,0x18)},
                {"refills",Read<std::uint16_t>(platform,0x38)},
                {"sampleRate",Read<std::uint32_t>(platform,0x3C)}};
        }
        std::uint32_t Init(void* init,void* platform)
        {
            // Only this verified game call site, before the engine has initialized.
            // Forward exactly once. Unknown/repeated inputs are observed, never altered.
            std::lock_guard lock(mutex);
            state["calls"]=state["calls"].get<unsigned>()+1;
            if (!init || !platform) {
                state["error"]="Unexpected null initialization arguments";
                return (*hook)(init,platform);
            }
            state["before"]=Fields(init,platform);
            const bool expected=state["calls"]==1 && state["before"]["samplesPerFrame"]==1024 &&
                state["before"]["refills"]==4 && state["before"]["sampleRate"]==48000;
            state["expectedInput"]=expected;
            if (expected && mode=="refills2") Write<std::uint16_t>(platform,0x38,2);
            if (expected && mode=="frame512") Write<std::uint32_t>(init,0x18,512);
            state["submitted"]=Fields(init,platform);
            const auto result=(*hook)(init,platform);
            state["result"]=result;
            // Init copies 0x78 bytes to 0x5D60880; platform copy helper copies
            // 0x48 bytes to 0x5D60CD0 and clamps refills <2 back to four.
            state["copied"]=Fields(reinterpret_cast<void*>(REL::Offset(0x5D60880).address()),
                reinterpret_cast<void*>(REL::Offset(0x5D60CD0).address()));
            state["rendererSamples"]=Read<std::uint16_t>(reinterpret_cast<void*>(REL::Offset(0x58628C4).address()),0);
            state["rendererRate"]=Read<std::uint32_t>(reinterpret_cast<void*>(REL::Offset(0x58628BC).address()),0);
            return result;
        }
    }
    void Install(const REL::Version& runtime)
    {
        try {
            std::ifstream input("Data/SFSE/Plugins/OSFAnimationTestHarness.json");
            if (!input) return;
            const auto config=Json::parse(input);
            if (!config.contains("audioBuffers")) return;
            mode=config.at("audioBuffers").get<std::string>();
            const auto utf8=config.at("stateDir").get<std::string>();
            const std::filesystem::path directory(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()),utf8.size()));
            const auto token=config.at("launchToken").get<std::string>();
            if (!directory.is_absolute() || directory==directory.root_path() || token.size()!=32 ||
                token.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos)
                throw std::runtime_error("Invalid private buffer experiment identity");
            if (mode!="observe" && mode!="refills2" && mode!="frame512")
                throw std::runtime_error("Unknown buffer experiment mode");
            state["enabled"]=true; state["mode"]=mode; state["launchToken"]=token;
            if (runtime!=REL::Version{1,16,244,0}) throw std::runtime_error("Unsupported game version");
            const auto site=REL::ID(73426).address()+0x3D3;
            const std::array<std::uint8_t,5> expected={0xE8,0xA8,0x74,0xDB,0x01};
            const std::array<std::uint8_t,15> prologue={0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x50};
            if (site!=REL::Offset(0xF1D473).address() ||
                std::memcmp(reinterpret_cast<void*>(site),expected.data(),expected.size()) ||
                REL::ID(150385).address()!=REL::Offset(0x2CD4920).address() ||
                std::memcmp(reinterpret_cast<void*>(REL::ID(150385).address()),prologue.data(),prologue.size()) ||
                Read<std::uint8_t>(reinterpret_cast<void*>(REL::Offset(0x5D60820).address()),0)!=0)
                throw std::runtime_error("Audio init byte/address/state gate rejected experiment");
            hook.emplace("[Audio] private D3 initialization",REL::ID(73426),0x3D3,&Init);
            if (!hook->Init() || !hook->Enable()) throw std::runtime_error("Audio init hook failed");
            state["installed"]=true;
        } catch(const std::exception& e) { state["error"]=e.what(); }
    }
    Json Snapshot() {
        std::lock_guard lock(mutex);
        auto result=state;
        if (state.value("result",0u)==1) {
            result["live"]=Fields(reinterpret_cast<void*>(REL::Offset(0x5D60880).address()),
                reinterpret_cast<void*>(REL::Offset(0x5D60CD0).address()));
            result["liveRendererSamples"]=Read<std::uint16_t>(reinterpret_cast<void*>(REL::Offset(0x58628C4).address()),0);
            result["liveRendererRate"]=Read<std::uint32_t>(reinterpret_cast<void*>(REL::Offset(0x58628BC).address()),0);
        }
        return result;
    }
}
#endif
