set_xmakever("3.0.0")
set_project("FusionMCP")
set_version("1.0.0")
set_languages("c17", "c++23")
set_warnings("allextra")
add_rules("mode.debug", "mode.releasedbg")

option("runtime")
    set_default(true)
    set_description("Build standalone F4SE DLL using an existing read-only CommonLib checkout")
option_end()
option("commonlib_root")
    set_default("lib/commonlibf4")
    set_description("Existing approved CommonLibF4 checkout; never fetched or modified by this project")
option_end()
if has_config("runtime") then
    local commonlib_root = path.absolute(get_config("commonlib_root") or "lib/commonlibf4", os.projectdir())
    if not os.isfile(path.join(commonlib_root, "xmake.lua")) then
        raise("Set --commonlib_root to an existing approved checkout, or --runtime=n for fixtures only")
    end
    includes(path.join(commonlib_root, "xmake.lua"))
    includes("extern/fusion-framework-fo4/xmake.lua")
end
target("fusionmcp_core")
    set_kind("static")
    add_files("src/core/*.cpp", "src/transport/Protocol.cpp")
    add_includedirs("src", "include", "vendor", {public=true})
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", {public=true})
    add_syslinks("bcrypt", "windowscodecs", "ole32", {public=true})

if has_config("runtime") then
target("FusionMCP")
    add_rules("commonlibf4.plugin", {
        name="FusionMCP", author="Bryant-21",
        description="Independent default-off authenticated local MCP host with optional providers",
        plugin_template=path.join(os.projectdir(), "res/commonlibf4-plugin.cpp.in")
    })
    set_basename("FusionMCP")
    add_deps("fusionmcp_core")
    fusionframework_use()
    add_files("src/runtime/ConfigFile.cpp", "src/runtime/Plugin.cpp", "src/runtime/NativeGameServices.cpp", "src/runtime/NativeWorldServices.cpp", "src/runtime/ConsoleHooks.cpp", "src/runtime/RenderDocTools.cpp", "src/runtime/PresentCapture.cpp", "src/transport/Server.cpp")
    add_defines("COMMONLIB_RUNTIMECOUNT=3", "UNICODE", "_UNICODE")
    add_syslinks("ws2_32", "shell32", "d3d11", "dxgi")
    add_shflags("/PDBALTPATH:FusionMCP.pdb", {force=true})
    add_installfiles("config/FusionMCP.toml", {prefixdir="F4SE/Plugins"})
end

