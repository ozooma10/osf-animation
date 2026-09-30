includes("lib/commonlibsf")

set_project("OSF Animation")
set_version("1.5.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

add_requires("fastgltf v0.9.0")
add_requires("ozz-animation 0.16.0")
add_requires("zlib 1.3.1")
add_requires("nlohmann_json 3.11.3")
add_requires("miniaudio 0.11.25") 

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

target("OSF Animation")
    add_rules("commonlibsf.plugin", {
        name = "OSF Animation",
        author = "ozooma10",
        description = "OSF Animation - native animations and scenes for Starfield",
        email = "98544147+ozooma10@users.noreply.github.com"
    })

    add_packages("fastgltf", "ozz-animation", "zlib", "nlohmann_json", "miniaudio")
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

    -- CommonLib deploys the DLL/PDB and these payloads through its install rule.
    add_installfiles("dist/Scripts/*.pex", { prefixdir = "Scripts" })
    add_installfiles("dist/Scripts/Source/*.psc", { prefixdir = "Scripts/Source" })
    add_installfiles("dist/OSF/internal.osf.json", { prefixdir = "OSF" })
    add_installfiles("dist/SFSE/Plugins/OSF/Settings/schemas/osf.animation.json", {
        prefixdir = "SFSE/Plugins/OSF/Settings/schemas"
    })

    -- The optional browser addon owns the view. Remove only its former locations
    -- in this mod; preserve other installed scripts, scenes, and MO2 metadata.
    after_build(function (target)
        local mods = os.getenv("XSE_SF_MODS_PATH")
        if mods then
            local plugins = path.join(mods, target:name(), "SFSE", "Plugins")
            os.tryrm(path.join(plugins, "OSF", "UI", "views", "osf.animation"))
            os.tryrm(path.join(plugins, "OSFUI", "views", "osf.animation"))
        end
    end)

-- Standalone GLTF import tester (xmake build osf-import-test).
target("osf-import-test")
    set_kind("binary")
    set_default(false)
    add_packages("fastgltf", "ozz-animation", "zlib", "nlohmann_json")
    add_files("src/Serialization/GLTFImport.cpp", "src/Util/Gzip.cpp", "test/ImportTest.cpp")
    add_includedirs("src")

-- Standalone .af import tester (xmake build osf-af-import-test).
-- Decodes a Starfield .af + skeleton.rig into ozz and samples a few poses, no game needed:
--   xmake run osf-af-import-test <clip.af> <skeleton.rig>
target("osf-af-import-test")
    set_kind("binary")
    set_default(false)
    add_packages("ozz-animation", "zlib")
    add_files("src/Serialization/AFImport.cpp", "src/Util/Ba2.cpp", "test/AFImportTest.cpp")
    add_includedirs("src")

-- Standalone shared-persistence broker tests (no game/CommonLib runtime needed).
target("osf-persistence-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("src/Serialization/PersistenceBroker.cpp", "test/PersistenceTest.cpp")
    add_includedirs("src")

-- Native scene-event callback registry tests (no game runtime needed).
target("osf-native-scene-event-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_deps("commonlibsf")
    add_files("src/API/NativeSceneEventRegistry.cpp",
              "test/NativeSceneEventTest.cpp")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

-- Pure shared-clock and clip-spec helper tests (no game/CommonLib runtime needed).
target("osf-frame-clock-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("src/Util/ClipPath.cpp", "test/FrameClockTest.cpp")
    add_includedirs("src")

-- Pure local-pose composition tests (reference-relative translation/rotation, weighting,
-- multiplication order, stage references, preservation, override compatibility, no accumulation)
-- plus the named bone-mask tables (BoneMask.h).
target("osf-additive-pose-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("test/AdditivePoseTest.cpp")
    add_includedirs("src")

-- Sound-pool parsing/subtitle fixtures and PCM-to-WEM serialization tests. Uses the CommonLib
-- logging surface but no game runtime.
target("osf-sound-registry-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_deps("commonlibsf")
    add_packages("nlohmann_json")
    add_files("src/Registry/SoundRegistry.cpp", "test/SoundRegistryTest.cpp")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
    set_rundir("test/fixtures")

-- Scene-registry parsing fixture tests (file-default roles, the roles registry: template
-- expansion, { id, ...overrides } merge, automatic runtime-name numbering, inference,
-- per-scene/per-file rejection). No game runtime: the fixtures resolve no form refs and the
-- clip-availability probe is stubbed "installed" in the test main.
target("osf-scene-registry-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_deps("commonlibsf")
    add_packages("nlohmann_json", "ozz-animation")
    add_files("src/Animation/Scene.cpp", "src/Registry/SceneRegistry.cpp", "src/Registry/SceneRegistryClips.cpp",
              "src/Util/ClipPath.cpp", "src/Util/Species.cpp", "test/SceneRegistryTest.cpp")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
    set_rundir("test/fixtures")

-- Pure overlay route/controller/compiler, playback-admission, owner-callback, and native-header tests.
target("osf-route-plan-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_deps("commonlibsf")
    add_packages("ozz-animation")
    add_files("src/Overlay/OwnerRegistry.cpp", "src/Overlay/RoutePlan.cpp",
              "src/Overlay/RoutePlaybackPlan.cpp", "test/RoutePlanTest.cpp")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
