includes("../xmake/plugin.lua")

target("PluginTemplate")
    add_rules("dd.plugin")
    set_version("1.0.0")

    set_pcxxheader("src/PCH.h")
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
target_end()
