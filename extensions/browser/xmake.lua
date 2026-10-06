target("OSF Animation Browser")
    add_rules("commonlibsf.plugin", {
        name = "OSF Animation Browser",
        author = "ozooma10",
        description = "Optional OSF Animation browser and emote wheel"
    })
    add_packages("nlohmann_json")
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src", "../../src/API")
    set_pcxxheader("src/pch.h")
target_end()
