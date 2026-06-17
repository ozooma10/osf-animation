-- include subprojects
includes("lib/commonlibsf")

-- set project constants
set_project("OSF Animation")
set_version("0.1.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add requirements
add_requires("fastgltf v0.9.0")
add_requires("ozz-animation 0.16.0")
add_requires("zlib")
add_requires("nlohmann_json")
add_requires("miniaudio 0.11.25")  -- loose-file cue playback (SoundService)

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- define targets
-- target name == repo folder == MO2 mod folder (deploy goes to XSE_SF_MODS_PATH\<target name>)
-- Final shipping name; repo == xmake target == MO2 mod (DESIGN.md §8).
target("OSF Animation")
    add_rules("commonlibsf.plugin", {
        name = "OSF Animation",
        author = "ozooma10",
        description = "OSF Animation — native animation playback core for Starfield",
        email = "98544147+ozooma10@users.noreply.github.com"
    })

    -- add dependency packages
    add_packages("fastgltf", "ozz-animation", "zlib", "nlohmann_json", "miniaudio")

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

    -- install the compiled papyrus scripts + animation pack json alongside the plugin
    after_build(function (target)
        local mods = os.getenv("XSE_SF_MODS_PATH")
        if mods then
            local scripts = path.join(mods, target:name(), "Scripts")
            local source = path.join(scripts, "Source")
            local osf = path.join(mods, target:name(), "OSF")
            os.mkdir(scripts)
            os.mkdir(source)
            os.mkdir(osf)
            os.cp("dist/Scripts/*.pex", scripts .. "/")
            os.cp("dist/Scripts/Source/*.psc", source .. "/")
            os.cp("dist/OSF/*.json", osf .. "/")
            os.cp("dist/OSF/Animations", osf .. "/")
            if os.isdir("dist/OSF/Sounds") then
                os.cp("dist/OSF/Sounds", osf .. "/")  -- sample/test cue sounds (SoundService)
            end
        end
    end)

-- offline GLTF import test harness (xmake build osf-import-test)
target("osf-import-test")
    set_kind("binary")
    set_default(false)
    add_packages("fastgltf", "ozz-animation", "zlib", "nlohmann_json")
    add_files("src/Serialization/GLTFImport.cpp", "test/ImportTest.cpp")
    add_includedirs("src")

