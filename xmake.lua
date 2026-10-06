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
    else
        remove_files("src/Testing/AnimationSmoke.cpp")
    end
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

    -- The browser addon owns the view; remove only its old locations in this mod.
    after_build(function (target)
        local mods = os.getenv("XSE_SF_MODS_PATH")
        if mods then
            local plugins = path.join(mods, target:name(), "SFSE", "Plugins")
            os.tryrm(path.join(plugins, "OSF", "UI", "views", "osf.animation"))
            os.tryrm(path.join(plugins, "OSFUI", "views", "osf.animation"))
        end
    end)

target("osf-import-test")
    set_kind("binary")
    set_default(false)
    add_packages("fastgltf", "ozz-animation", "zlib", "nlohmann_json")
    add_files("src/Serialization/GLTFImport.cpp", "src/Util/Gzip.cpp", "test/ImportTest.cpp")
    add_includedirs("src")

-- xmake run osf-af-import-test <clip.af> <skeleton.rig>
target("osf-af-import-test")
    set_kind("binary")
    set_default(false)
    add_packages("ozz-animation", "zlib")
    add_files("src/Serialization/AFImport.cpp", "src/Util/Ba2.cpp", "test/AFImportTest.cpp")
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

target("osf-frame-clock-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("src/Util/ClipPath.cpp", "src/Util/NativeMainThreadQueue.cpp", "test/FrameClockTest.cpp")
    add_includedirs("src")

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
    add_files("src/Registry/SoundRegistry.cpp", "test/SoundRegistryTest.cpp")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
    set_rundir("test/fixtures")

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

target("osf-import-smoke-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_packages("fastgltf", "ozz-animation", "zlib", "nlohmann_json")
    add_files("src/Serialization/GLTFImport.cpp", "src/Util/Gzip.cpp", "test/ImportSmokeTest.cpp")
    add_includedirs("src")
