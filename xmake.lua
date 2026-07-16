includes("lib/commonlibsf")

set_project("OSF Animation")
set_version("0.1.1")
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

-- The target name doubles as the MO2 mod folder, so a build deploys straight to XSE_SF_MODS_PATH\OSF Animation.
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

    -- Copy the compiled DLL and Papyrus scripts into the mod folder.
    after_build(function (target)
        local mods = os.getenv("XSE_SF_MODS_PATH")
        if mods then
            -- SFSE loads plugins from <mod>\SFSE\Plugins\*.dll.
            local plugins = path.join(mods, target:name(), "SFSE", "Plugins")
            local scripts = path.join(mods, target:name(), "Scripts")
            local osfDir = path.join(mods, target:name(), "OSF")
            local source = path.join(scripts, "Source")
            os.tryrm(osfDir)
            os.tryrm(scripts)
            os.mkdir(plugins)
            os.mkdir(scripts)
            os.mkdir(source)
            os.cp("dist/Scripts/*.pex", scripts .. "/")
            os.cp("dist/Scripts/Source/*.psc", source .. "/")
            os.cp("dist/OSF/**", osfDir .. "/", { rootdir = "dist/OSF" })
            -- No settings.json: settings + hotkeys live in OSF UI's settings menu
            -- (UISettings.cpp registers the schema over the bridge at runtime).

            local view = path.join(plugins, "OSFUI", "views", "osf")
            os.tryrm(view)
            os.mkdir(view)
            os.cp("views/osf/manifest.json", view .. "/")
            os.cp("views/osf/index.html", view .. "/")
            os.cp("views/osf/main.js", view .. "/")
            os.cp("views/osf/style.css", view .. "/")
            local ok = try { function() os.cp(target:targetfile(), plugins .. "/"); return true end }
            if ok then
                if os.isfile(target:symbolfile()) then
                    os.cp(target:symbolfile(), plugins .. "/")  -- .pdb for crash-log symbolication
                end
            else
                print("[OSF] DLL is busy (game running?) — scripts/scenes deployed, DLL NOT updated. Close the game and rebuild to update the DLL.")
            end
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
    set_languages("c++23")
    add_packages("ozz-animation", "zlib")
    add_files("src/Serialization/AFImport.cpp", "src/Util/Ba2.cpp", "test/AFImportTest.cpp")
    add_includedirs("src")
