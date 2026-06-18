includes("lib/commonlibsf")

set_project("OSF Animation")
set_version("0.1.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

add_requires("fastgltf v0.9.0")
add_requires("ozz-animation 0.16.0")
add_requires("zlib")
add_requires("nlohmann_json")
add_requires("miniaudio 0.11.25")  -- plays loose-file sound cues

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- The target name doubles as the MO2 mod folder, so a build deploys
-- straight to XSE_SF_MODS_PATH\OSF Animation.
target("OSF Animation")
    add_rules("commonlibsf.plugin", {
        name = "OSF Animation",
        author = "ozooma10",
        description = "OSF Animation — native animation playback core for Starfield",
        email = "98544147+ozooma10@users.noreply.github.com"
    })

    add_packages("fastgltf", "ozz-animation", "zlib", "nlohmann_json", "miniaudio")

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

    -- Copy the compiled Papyrus scripts and animation JSON next to the plugin in the mod folder.
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
                os.cp("dist/OSF/Sounds", osf .. "/")  -- sample sound cues, if any are present
            end
            -- No soundbanks ship: loose audio plays through a SHIPPED event's external-source
            -- slot (WwiseBackend), not a mod .bnk — the engine resolves banks by BSResource
            -- registry membership, so a custom LoadBank fails (OSF RE, 1.16.244).
        end
    end)

-- Standalone GLTF import tester you can run without the game (xmake build osf-import-test).
target("osf-import-test")
    set_kind("binary")
    set_default(false)
    add_packages("fastgltf", "ozz-animation", "zlib", "nlohmann_json")
    add_files("src/Serialization/GLTFImport.cpp", "test/ImportTest.cpp")
    add_includedirs("src")

-- Offline unit tests for the engine-independent logic (registries, matchmaker,
-- util, scene math). Builds WITHOUT CommonLibSF/the game by force-including the
-- RE/REX stub pch (test/stubs/test_pch.h). Run with: xmake build osf-tests &&
-- xmake run osf-tests.  See test/README.md.
target("osf-tests")
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    set_warnings("less")  -- the stubs trip allextra; the real target keeps allextra
    add_packages("ozz-animation", "nlohmann_json")

    -- The real, engine-independent logic under test...
    add_files("src/Registry/PackRegistry.cpp")
    add_files("src/Registry/SceneRegistry.cpp")
    add_files("src/Matchmaking/Matchmaker.cpp")
    -- ...plus the test cases and runner.
    add_files("test/unit/*.cpp")

    add_includedirs("src", "test")
    set_pcxxheader("test/stubs/test_pch.h")

    -- Bake the fixtures path so the runner can chdir there with no arguments.
    on_load(function (target)
        local fixtures = path.join(os.projectdir(), "test", "fixtures"):gsub("\\", "/")
        target:add("defines", "OSF_TEST_FIXTURES_DIR=\"" .. fixtures .. "\"")
    end)

