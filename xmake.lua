includes("lib/commonlibsf")

-- CommonLib's automatic install defaults to every target. With two independent DLLs,
-- deploy only the target that just finished (the other may not have linked yet).
rule("commonlib.plugin")
    after_build(function (target)
        if target:installdir() then
            import("core.project.task")
            task.run("install", {target = target:name()})
        end
    end)
rule_end()

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
add_includedirs("shared")

option("animation_smoke")
    set_default(false)
    set_showmenu(true)
    set_description("Enable the private development-only Animation smoke mailbox")
option_end()

target("OSF Animation")
    add_rules("commonlibsf.plugin", {
        name = "OSF Animation",
        author = "ozooma10",
        description = "OSF Animation - native animations and scenes for Starfield",
        email = "98544147+ozooma10@users.noreply.github.com"
    })

    add_packages("fastgltf", "ozz-animation", "zlib", "nlohmann_json", "miniaudio")
    add_files("src/**.cpp")
    if has_config("animation_smoke") then
        add_defines("OSF_ANIMATION_SMOKE")
        add_includedirs("extensions/browser/sdk")
    else
        remove_files("src/Testing/AnimationSmoke.cpp")
    end
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

    -- CommonLib deploys the DLL/PDB and these payloads through its install rule.
    for _, script in ipairs({"OSF", "OSFTypes", "OSFAdvanced"}) do
        add_installfiles("dist/Scripts/" .. script .. ".pex", { prefixdir = "Scripts" })
        add_installfiles("dist/Scripts/Source/" .. script .. ".psc", { prefixdir = "Scripts/Source" })
    end
    add_installfiles("dist/OSF/internal.osf.json", { prefixdir = "OSF" })
    add_installfiles("dist/SFSE/Plugins/OSF/Settings/schemas/osf.animation.engine.json", {
        prefixdir = "SFSE/Plugins/OSF/Settings/schemas"
    })

    -- The browser addon owns the view; remove only its old locations in this mod.
    after_build(function (target)
        local mods = os.getenv("XSE_SF_MODS_PATH")
        if mods then
            local plugins = path.join(mods, target:name(), "SFSE", "Plugins")
            -- Both components now ship explicit engine/browser schema names.
            os.tryrm(path.join(plugins, "OSF", "Settings", "schemas", "osf.animation.json"))
            os.tryrm(path.join(plugins, "OSF", "UI", "views", "osf.animation"))
            os.tryrm(path.join(plugins, "OSFUI", "views", "osf.animation"))
        end
    end)

target_end()

includes("extensions/browser")

target("osf-import-test")
    set_kind("binary")
    set_default(false)
    add_packages("fastgltf", "ozz-animation", "zlib", "nlohmann_json")
    add_files("src/Serialization/GLTFImport.cpp", "src/Util/Gzip.cpp", "test/ImportTest.cpp")
    add_includedirs("src")

target("osf-browser-boundary-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_deps("commonlibsf")
    add_packages("nlohmann_json")
    add_files("test/BrowserBoundaryTest.cpp", "src/Util/NativeMainThreadQueue.cpp",
              "extensions/browser/src/API/EngineClient.cpp", "extensions/browser/src/API/UIBridgeCatalog.cpp",
              "extensions/browser/src/API/BrowserClipLibrary.cpp", "test/BrowserClipLibraryTest.cpp",
              "extensions/browser/src/Serialization/ClipDurationCache.cpp")
    add_includedirs("extensions/browser/src", "src", "src/API")
    set_pcxxheader("extensions/browser/src/pch.h")

-- xmake run osf-af-import-test <clip.af> <skeleton.rig>
target("osf-af-import-test")
    set_kind("binary")
    set_default(false)
    add_packages("ozz-animation", "zlib")
    add_files("src/Serialization/AFImport.cpp", "src/Util/Ba2.cpp", "test/AFImportTest.cpp")
    add_includedirs("src")

target("osf-af-fixture-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_packages("ozz-animation", "zlib")
    add_files("src/Serialization/AFImport.cpp", "src/Util/Ba2.cpp", "test/AFFixtureTest.cpp")
    add_includedirs("src")

target("osf-persistence-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("src/Serialization/PersistenceBroker.cpp", "test/PersistenceTest.cpp")
    add_includedirs("src")

target("osf-native-scene-event-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_deps("commonlibsf")
    add_files("src/API/NativeSceneEventRegistry.cpp",
              "test/NativeSceneEventTest.cpp")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

target("osf-scene-launcher-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_deps("commonlibsf")
    add_packages("nlohmann_json", "ozz-animation")
    add_files("src/Scene/SceneLauncher.cpp", "test/SceneLauncherTest.cpp")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

target("osf-frame-clock-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("src/Util/ClipPath.cpp", "src/Util/NativeMainThreadQueue.cpp", "test/FrameClockTest.cpp")
    add_includedirs("src")

target("osf-playback-session-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_deps("commonlibsf")
    add_packages("ozz-animation", "nlohmann_json")
    add_files("src/Animation/Scene.cpp", "src/Preparation/Coordinator.cpp", "test/PlaybackSessionTest.cpp")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

target("osf-clip-duration-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_deps("commonlibsf")
    add_packages("nlohmann_json", "ozz-animation", "zlib")
    add_files("src/Serialization/ClipDurations.cpp", "src/Serialization/ClipDurationProbe.cpp",
              "extensions/browser/src/Serialization/ClipDurationCache.cpp",
              "src/Util/ClipPath.cpp", "src/Util/Gzip.cpp", "test/ClipDurationTest.cpp")
    add_includedirs("extensions/browser/src", "src", "src/API")
    set_pcxxheader("src/pch.h")

target("osf-additive-pose-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("test/AdditivePoseTest.cpp")
    add_includedirs("src")

target("osf-sound-registry-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_deps("commonlibsf")
    add_packages("nlohmann_json")
    add_defines("OSF_AUDIO_TEST")
    add_files("src/Registry/SoundRegistry.cpp", "src/Audio/SoundService.cpp", "test/SoundRegistryTest.cpp", "test/SoundServiceTest.cpp")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
    set_rundir("test/fixtures")

target("osf-scene-registry-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_deps("commonlibsf")
    add_packages("nlohmann_json", "ozz-animation")
    add_files("src/Animation/Scene.cpp", "src/Registry/ContentRegistry.cpp", "src/Registry/ContentRegistryClips.cpp", "src/Registry/ContentProviders.cpp",
              "src/Util/ClipPath.cpp", "src/Util/Species.cpp", "src/API/CatalogBuilder.cpp",
              "src/Serialization/ClipDurations.cpp", "test/ContentRegistryTest.cpp", "test/CatalogBuilderTest.cpp", "test/ContentProvidersTest.cpp",
              "test/LoopGlobalsTest.cpp")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
    set_rundir("test/fixtures")

target("osf-import-smoke-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_packages("fastgltf", "ozz-animation", "zlib", "nlohmann_json")
    add_files("src/Serialization/GLTFImport.cpp", "src/Util/Gzip.cpp", "test/ImportSmokeTest.cpp")
    add_includedirs("src")

target("osf-actor-state-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("src/ActorState/Store.cpp", "src/Preparation/Coordinator.cpp", "test/ActorStateTest.cpp")
    add_includedirs("src")

target("osf-preparation-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("src/Preparation/Coordinator.cpp", "test/PreparationTest.cpp")
    add_includedirs("src")
