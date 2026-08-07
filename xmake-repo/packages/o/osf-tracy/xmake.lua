package("osf-tracy")
    set_homepage("https://github.com/wolfpld/tracy")
    set_description("Pinned Tracy client for OSF Animation profiling builds")
    set_license("BSD-3-Clause")

    add_urls("https://github.com/wolfpld/tracy/archive/refs/tags/$(version).tar.gz")
    add_versions("v0.13.1", "d4efc50ebcb0bfcfdbba148995aeb75044c0d80f5d91223aebfaa8fa9e563d2b")

    add_configs("on_demand",         { type = "boolean", default = true })
    add_configs("only_localhost",    { type = "boolean", default = true })
    add_configs("broadcast",         { type = "boolean", default = false })
    add_configs("crash_handler",     { type = "boolean", default = false })
    add_configs("enforce_callstack", { type = "boolean", default = false })
    add_configs("callstack",         { type = "boolean", default = false })
    add_configs("code_transfer",     { type = "boolean", default = false })
    add_configs("context_switch",    { type = "boolean", default = false })
    add_configs("sampling",          { type = "boolean", default = false })
    add_configs("vsync_capture",     { type = "boolean", default = false })
    add_configs("frame_image",       { type = "boolean", default = false })
    add_configs("system_tracing",    { type = "boolean", default = false })
    add_configs("fibers",            { type = "boolean", default = false })

    add_deps("cmake")
    add_includedirs("include/tracy")
    if is_plat("windows", "mingw") then
        add_syslinks("ws2_32", "dbghelp")
    end

    on_install(function (package)
        local function flag(value)
            return value and "ON" or "OFF"
        end

        local configs = {
            "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"),
            "-DBUILD_SHARED_LIBS=OFF",
            "-DTRACY_STATIC=ON",
            "-DTRACY_Fortran=OFF",
            "-DTRACY_LTO=OFF",
            "-DTRACY_ENABLE=ON",
            "-DTRACY_ON_DEMAND=" .. flag(package:config("on_demand")),
            "-DTRACY_ONLY_LOCALHOST=" .. flag(package:config("only_localhost")),
            "-DTRACY_NO_BROADCAST=" .. flag(not package:config("broadcast")),
            "-DTRACY_NO_CRASH_HANDLER=" .. flag(not package:config("crash_handler")),
            "-DTRACY_CALLSTACK=" .. flag(package:config("enforce_callstack")),
            "-DTRACY_NO_CALLSTACK=" .. flag(not package:config("callstack")),
            "-DTRACY_NO_CODE_TRANSFER=" .. flag(not package:config("code_transfer")),
            "-DTRACY_NO_CONTEXT_SWITCH=" .. flag(not package:config("context_switch")),
            "-DTRACY_NO_SAMPLING=" .. flag(not package:config("sampling")),
            "-DTRACY_NO_VSYNC_CAPTURE=" .. flag(not package:config("vsync_capture")),
            "-DTRACY_NO_FRAME_IMAGE=" .. flag(not package:config("frame_image")),
            "-DTRACY_NO_SYSTEM_TRACING=" .. flag(not package:config("system_tracing")),
            "-DTRACY_FIBERS=" .. flag(package:config("fibers"))
        }

        -- CMake publishes these definitions to the Tracy client. Mirror every
        -- enabled definition onto consumers so Tracy.hpp and TracyClient.cpp
        -- are compiled with an identical feature set.
        for _, config in ipairs(configs) do
            local define, value = config:match("-D(TRACY_%S+)=(.*)")
            if define and value == "ON" then
                package:add("defines", define)
            end
        end

        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        assert(package:check_cxxsnippets({ test = [[
            #include <tracy/Tracy.hpp>
            void test() {
                ZoneScopedN("osf-tracy-package-test");
                TracyPlot("osf-tracy-package-test", int64_t{ 1 });
            }
        ]] }, { configs = { languages = "c++17" } }))
    end)
