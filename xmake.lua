includes("lib/commonlibsf")

set_project("OSF Animation")
set_version("1.5.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

option("osf_profiler")
    set_default(false)
    set_showmenu(true)
    set_description("Build the development-only Tracy profiling DLL")
option_end()

add_requires("fastgltf v0.9.0")
add_requires("ozz-animation 0.16.0")
add_requires("zlib 1.3.1")
add_requires("nlohmann_json 3.11.3")
add_requires("miniaudio 0.11.25") 
if has_config("osf_profiler") then
    -- Keep the profiler dependency fully outside normal package resolution. The
    -- project-local recipe pins Tracy's source archive and avoids drift in the
    -- rolling xmake-repo recipe.
    add_repositories("osf-local xmake-repo")
    add_requires("osf-tracy v0.13.1", { configs = {
        on_demand = true,
        only_localhost = true,
        broadcast = false,
        crash_handler = false,
        enforce_callstack = false,
        callstack = false,
        code_transfer = false,
        context_switch = false,
        sampling = false,
        vsync_capture = false,
        frame_image = false,
        system_tracing = false,
        fibers = false
    } })
end

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- CommonLibSF derives its MO2 install directory from the target name. Give the
-- profiling build a distinct target so its automatic install step can never
-- touch the normal mod, while preserving the SFSE DLL filename.
local plugin_target = has_config("osf_profiler") and "OSF Animation Profiling" or "OSF Animation"
target(plugin_target)
    if has_config("osf_profiler") then
        set_basename("OSF Animation")
    end
    add_rules("commonlibsf.plugin", {
        name = "OSF Animation",
        author = "ozooma10",
        description = "OSF Animation - native animations and scenes for Starfield",
        email = "98544147+ozooma10@users.noreply.github.com"
    })

    add_packages("fastgltf", "ozz-animation", "zlib", "nlohmann_json", "miniaudio")
    set_values("osf.profiler", has_config("osf_profiler"))
    if has_config("osf_profiler") then
        add_packages("osf-tracy")
        add_defines("OSF_ENABLE_PROFILING=1", "OSF_TRACY_PROFILE_BUILD=1")
    else
        add_defines("OSF_ENABLE_PROFILING=0")
    end

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

    -- Rebuild the browser view with the OSF UI authoring CLI when its sources are newer
    -- than the generated output, so a plain `xmake` never deploys a stale UI.
    -- The output is generated, never committed: npm is required to build this target.
    before_build(function (target)
        import("lib.detect.find_tool")
        import("core.base.option")

        -- The profiling override contains only the native DLL/PDB and inherits all
        -- content/UI from the ordinary OSF Animation mod through MO2's VFS.
        if target:values("osf.profiler") then
            return
        end

        local ui = "ui/animation-browser"
        local view = "build/osfui-animation-browser/SFSE/Plugins/OSFUI/views/osf.animation/browser"
        local out = path.join(view, "index.html")

        -- Newest mtime across everything the bundle is built from.
        local newest = 0
        local inputs = os.files(path.join(ui, "src/**")) or {}
        table.join2(inputs, os.files(path.join(ui, "public/**")) or {})
        table.join2(inputs, { path.join(ui, "package.json"), path.join(ui, "package-lock.json"),
                              path.join(ui, "osfui.config.ts"), path.join(ui, "osfui.mock.ts"),
                              path.join(ui, "tsconfig.json") })
        for _, file in ipairs(inputs) do
            if os.isfile(file) then
                newest = math.max(newest, os.mtime(file))
            end
        end

        local fresh = os.isfile(out) and os.mtime(out) >= newest
        if fresh and not option.get("rebuild") then
            return
        end

        -- npm ships as npm.cmd on Windows; find_tool resolves either.
        local npm = find_tool("npm.cmd") or find_tool("npm")
        if not npm then
            if os.isfile(out) then
                print("[OSF] npm not found — deploying the existing browser build (may be stale).")
                return
            end
            raise("[OSF] npm not found and no generated browser view exists — install Node.js to build the animation browser.")
        end

        if not os.isdir(path.join(ui, "node_modules")) then
            print("[OSF] installing animation-browser dependencies (npm ci)...")
            os.vrunv(npm.program, { "ci" }, { curdir = ui })
        end

        print("[OSF] building the animation browser (npm run build)...")
        os.vrunv(npm.program, { "run", "build" }, { curdir = ui })
    end)

    -- Copy the compiled DLL and Papyrus scripts into the mod folder.
    after_build(function (target)
        local mods = os.getenv("XSE_SF_MODS_PATH")
        if mods then
            if target:values("osf.profiler") then
                local profileRoot = path.join(mods, "OSF Animation Profiling")
                local plugins = path.join(profileRoot, "SFSE", "Plugins")
                os.mkdir(plugins)
                local ok = try { function() os.cp(target:targetfile(), plugins .. "/"); return true end }
                if not ok then
                    print("[OSF] profiling DLL is busy (game running?) — override NOT updated. Close the game and rebuild.")
                    return
                end
                if os.isfile(target:symbolfile()) then
                    os.cp(target:symbolfile(), plugins .. "/")
                end
                io.writefile(path.join(profileRoot, "meta.ini"), [[
[General]
gameName=Starfield
modid=0
version=]] .. (target:version() or "dev") .. [[-profile
category="-1,"
comments=Development-only Tracy override for OSF Animation. Enable at higher conflict priority than the normal mod.
notes=Contains only OSF Animation.dll and its PDB. Disable after profiling.
]])
                print("[OSF] deployed development-only profiling override to " .. profileRoot)
                return
            end

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
            local pexFiles = os.files("dist/Scripts/*.pex") or {}
            if #pexFiles > 0 then
                os.cp("dist/Scripts/*.pex", scripts .. "/")
            else
                print("[OSF] no compiled Papyrus scripts found in dist/Scripts — compile Papyrus first; no .pex files were deployed.")
            end
            os.cp("dist/Scripts/Source/*.psc", source .. "/")
            os.cp("dist/OSF/**", osfDir .. "/", { rootdir = "dist/OSF" })
            -- No settings.json: settings + hotkeys live in OSF UI's settings menu
            -- (UISettings.cpp registers the schema over the bridge at runtime).

            -- Nested namespace layout (OSF UI api-freeze item 1):
            -- views/<modId>/<viewName>/ with the qualified id "osf.animation/browser".
            -- Wholesale: this mod owns the tree, and the CLI decides what its
            -- top-level entries are called (naming them here goes stale silently,
            -- leaving orphans behind — including the pre-rename dotless "osf").
            local views = path.join(plugins, "OSFUI", "views")
            os.tryrm(views)
            os.mkdir(views)
            -- The browser is generated by the OSF UI CLI (before_build refreshes it).
            -- Copy its complete views tree: the generated HTML references hashed assets
            -- at the namespace root and the CLI also installs the shared authoring kit.
            local builtViews = "build/osfui-animation-browser/SFSE/Plugins/OSFUI/views"
            os.cp(path.join(builtViews, "**"), views .. "/", { rootdir = builtViews })
            -- The settings-card icon is a static asset, not a bundle input: the CLI
            -- roots Vite at src/views and emits only what the entry references, so
            -- copy it in verbatim. UISettings.cpp's schema "icon" is resolved by the
            -- host relative to the views root, hence <modId>/<viewName>/ here.
            os.cp(path.join("ui/animation-browser/public", "osf-icon.svg"),
                  path.join(views, "osf.animation", "browser") .. "/")
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
    add_defines("OSF_ENABLE_PROFILING=0")
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

-- Scene-registry parsing fixture tests (pack-default roles, the roles registry: template
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
