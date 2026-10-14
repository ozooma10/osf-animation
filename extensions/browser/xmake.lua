target("OSF Animation Browser")
    add_rules("commonlibsf.plugin", {
        name = "OSF Animation Browser",
        author = "ozooma10",
        description = "Optional OSF Animation browser and emote wheel"
    })
    add_packages("nlohmann_json")
    add_files("src/**.cpp")
    if has_config("animation_smoke") then
        add_defines("OSF_ANIMATION_SMOKE")
    end
    add_headerfiles("src/**.h")
    add_includedirs("src", "sdk", "../../src/API")
    set_pcxxheader("src/pch.h")
    add_installfiles("dist/SFSE/Plugins/OSF/Settings/schemas/osf.animation.browser.json", {
        prefixdir = "SFSE/Plugins/OSF/Settings/schemas"
    })
    after_build(function (target)
        local mods = os.getenv("XSE_SF_MODS_PATH")
        if mods then
            os.tryrm(path.join(mods, target:name(), "SFSE", "Plugins", "OSF", "Settings", "schemas", "osf.animation.json"))
        end
    end)
target_end()
